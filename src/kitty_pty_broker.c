#define _GNU_SOURCE

#include "kitty_pty_broker.h"
#include "internal.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

typedef struct {
    int error_number;
    int64_t broker_pid;
    int64_t child_pid;
} server_ready;

typedef struct {
    char session_id[KPB_SESSION_ID_MAX + 1];
    char runtime_dir[KPB_PATH_MAX];
    char sessions_dir[KPB_PATH_MAX];
    char reaped_dir[KPB_PATH_MAX];
    char session_dir[KPB_PATH_MAX];
    char socket_path[KPB_PATH_MAX];
    char journal_path[KPB_PATH_MAX];
    char metadata_path[KPB_PATH_MAX];
} session_paths;

/* A bounded queue of encoded frames awaiting a non-blocking socket.
 *
 * Every attached peer is written through one of these, because a peer that
 * stops reading must never hold the event loop: the loop is also the control
 * plane, and `kill` has to keep working precisely when a frontend has stopped
 * behaving.  What differs per peer is only the overflow policy - an observer
 * that falls behind is disconnected, while the read-write client's backlog
 * stops the PTY from being read, so the kernel's own buffer stops the shell. */
typedef struct {
    unsigned char *data;
    size_t offset;
    size_t size;
    size_t capacity;
} frame_queue;

/* An attached read-only observer.
 *
 * An observer must never be able to stall the pane, so one that falls behind
 * is disconnected rather than buffered without bound.  That is cheap because
 * a dropped observer can reattach and resume from the offset it had reached.
 *
 * `generation` is bumped whenever a slot is released, so a poll result
 * captured before the slot was recycled can be recognised as stale. */
typedef struct {
    int fd;
    frame_queue out;
    unsigned char in[sizeof(kpb_frame_header)];
    size_t in_size;
    uint32_t generation;
} observer_slot;

/* Strictly above KPB_OBSERVER_REPLAY_MAX: a fresh replay must never by itself
 * fill the queue and trip the drop policy, or observers would be disconnected
 * at the moment they attach. */
#define KPB_OBSERVER_QUEUE_LIMIT (2U * 1024U * 1024U)
_Static_assert(
    KPB_OBSERVER_REPLAY_MAX < KPB_OBSERVER_QUEUE_LIMIT,
    "a fresh observer replay must fit in the queue with headroom to spare");

/* The read-write client's queue.  Past the high-water mark the loop stops
 * polling the PTY for output, so the kernel PTY buffer fills and the shell
 * blocks in write() - the same backpressure the old blocking send provided,
 * without handing the client the power to stop the broker's control plane.
 * The headroom above the mark absorbs the chunks already in flight. */
#define KPB_CLIENT_QUEUE_LIMIT (4U * 1024U * 1024U)
#define KPB_CLIENT_QUEUE_HIGH_WATER (2U * 1024U * 1024U)
_Static_assert(
    KPB_CLIENT_QUEUE_HIGH_WATER + KPB_IO_CHUNK + sizeof(kpb_frame_header) <
        KPB_CLIENT_QUEUE_LIMIT,
    "the client queue must absorb a full PTY chunk beyond the high-water mark");

typedef struct {
    session_paths paths;
    char session_id[KPB_SESSION_ID_MAX + 1];
    char cwd[KPB_PATH_MAX];
    char command[KPB_COMMAND_MAX];
    int listener_fd;
    int pty_fd;
    int journal_fd;
    int transcript_fd;
    int client_fd;
    pid_t child_pid;
    uint64_t started_millis;
    uint64_t journal_bytes;
    uint64_t journal_epoch;
    uint64_t journal_limit;
    bool journal_complete;
    uint64_t transcript_bytes;
    uint64_t transcript_limit;
    int transcript_graphics;
    int transcript_scan;
    uint64_t transcript_elided;
    bool terminate_requested;
    uint64_t terminate_deadline;
    frame_queue client_out;
    bool client_replay_active;
    uint64_t client_replay_at;
    uint64_t client_replay_epoch;
    struct winsize size;
    unsigned char *input_buffer;
    size_t input_offset;
    size_t input_size;
    size_t input_capacity;
    observer_slot observers[KPB_OBSERVER_MAX];
    size_t observer_count;
    uint32_t observer_generation;
} server_state;

#define KPB_INPUT_BUFFER_LIMIT (16U * 1024U * 1024U)

static uint64_t
host_to_be64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

static uint64_t
be64_to_host(uint64_t value) {
    return host_to_be64(value);
}

static int
copy_string(char *destination, size_t capacity, const char *source) {
    size_t size;
    if (!destination || capacity == 0 || !source) return -1;
    size = strlen(source);
    if (size >= capacity) return -1;
    memcpy(destination, source, size + 1);
    return 0;
}

static int
copy_wire_string(
    char *destination,
    size_t destination_capacity,
    const char *source,
    size_t source_capacity
) {
    const char *end;
    size_t size;
    if (!destination || !destination_capacity || !source) return -1;
    end = memchr(source, '\0', source_capacity);
    if (!end) return -1;
    size = (size_t)(end - source);
    if (size >= destination_capacity) return -1;
    memcpy(destination, source, size + 1);
    return 0;
}

static int
join_path(char *output, size_t capacity, const char *left, const char *right) {
    int result;
    if (!left || !right || !*left || !*right) return -1;
    result = snprintf(output, capacity, "%s/%s", left, right);
    return result < 0 || (size_t)result >= capacity ? -1 : 0;
}

static bool
valid_component(const char *value) {
    size_t index;
    if (!value || !*value) return false;
    for (index = 0; value[index]; index++) {
        unsigned char c = (unsigned char)value[index];
        if (index >= KPB_SESSION_ID_MAX) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return strcmp(value, ".") != 0 && strcmp(value, "..") != 0;
}

static kpb_result
ensure_private_directory(const char *path, bool create) {
    struct stat status;
    if (!path || path[0] != '/') return KPB_ERR_INVALID;
    if (lstat(path, &status) != 0) {
        if (errno != ENOENT || !create) return errno == ENOENT ? KPB_ERR_NOT_FOUND : KPB_ERR_SYSTEM;
        if (mkdir(path, 0700) != 0) return KPB_ERR_SYSTEM;
        if (lstat(path, &status) != 0) return KPB_ERR_SYSTEM;
    }
    if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode) || status.st_uid != geteuid()) {
        return KPB_ERR_SECURITY;
    }
    if ((status.st_mode & 077) != 0 && chmod(path, 0700) != 0) return KPB_ERR_SYSTEM;
    return KPB_OK;
}

/* The runtime directory as the kernel will see it, whether or not it exists
 * yet.  realpath() fails outright on a path that does not exist, which would
 * leave the caller measuring an unresolved path; resolving the longest existing
 * ancestor and appending the rest gives the same answer the directory will
 * have once it is created, so a path that is too long is refused BEFORE
 * anything is made. */
static kpb_result
resolve_runtime(const char *runtime_dir, char output[KPB_PATH_MAX]) {
    char prefix[KPB_PATH_MAX];
    char resolved[KPB_PATH_MAX];
    size_t length = strlen(runtime_dir);
    size_t end = length;
    const char *suffix;
    int count;
    if (length >= KPB_PATH_MAX) return KPB_ERR_NAME_TOO_LONG;
    for (;;) {
        if (end == 0) {
            strcpy(prefix, "/");
        } else {
            memcpy(prefix, runtime_dir, end);
            prefix[end] = '\0';
        }
        if (realpath(prefix, resolved)) break;
        if (errno != ENOENT && errno != ENOTDIR) return KPB_ERR_SYSTEM;
        if (end == 0) return KPB_ERR_SYSTEM;
        while (end > 0 && runtime_dir[end - 1] != '/') end--;
        if (end > 0) end--;
    }
    suffix = runtime_dir + end;
    if (*suffix && (strstr(suffix, "/../") || strstr(suffix, "/./") ||
                    (length >= 3 && strcmp(runtime_dir + length - 3, "/..") == 0) ||
                    (length >= 2 && strcmp(runtime_dir + length - 2, "/.") == 0))) {
        return KPB_ERR_INVALID;
    }
    count = snprintf(
        output, KPB_PATH_MAX, "%s%s",
        strcmp(resolved, "/") == 0 ? "" : resolved, suffix);
    if (count < 0 || count >= KPB_PATH_MAX) return KPB_ERR_NAME_TOO_LONG;
    if (output[0] == '\0') strcpy(output, "/");
    return KPB_OK;
}

static kpb_result
build_paths(const char *runtime_dir, const char *session_id, session_paths *paths) {
    char resolved[KPB_PATH_MAX];
    kpb_result result;
    if (!runtime_dir || runtime_dir[0] != '/' || !paths) return KPB_ERR_INVALID;
    result = resolve_runtime(runtime_dir, resolved);
    if (result != KPB_OK) return result;
    memset(paths, 0, sizeof *paths);
    if (copy_string(paths->runtime_dir, sizeof paths->runtime_dir, resolved) != 0 ||
        join_path(paths->sessions_dir, sizeof paths->sessions_dir, resolved, "sessions") != 0 ||
        join_path(paths->reaped_dir, sizeof paths->reaped_dir, resolved, "reaped") != 0) {
        return KPB_ERR_NAME_TOO_LONG;
    }
    if (!session_id) return KPB_OK;
    if (!valid_component(session_id)) return KPB_ERR_INVALID;
    copy_string(paths->session_id, sizeof paths->session_id, session_id);
    if (join_path(paths->session_dir, sizeof paths->session_dir, paths->sessions_dir, session_id) != 0 ||
        join_path(paths->socket_path, sizeof paths->socket_path, paths->session_dir, "control.sock") != 0 ||
        join_path(paths->journal_path, sizeof paths->journal_path, paths->session_dir, "journal.bin") != 0 ||
        join_path(paths->metadata_path, sizeof paths->metadata_path, paths->session_dir, "metadata") != 0) {
        return KPB_ERR_NAME_TOO_LONG;
    }
    if (strlen(paths->socket_path) > KPB_SOCKET_PATH_LIMIT) return KPB_ERR_NAME_TOO_LONG;
    return KPB_OK;
}

/* A deadline `millis` from now, normalised.  One place, because open-coding
 * the tv_nsec carry at each call site is how one of them ends up wrong. */
static void
deadline_in(struct timespec *out, long millis) {
    clock_gettime(CLOCK_MONOTONIC, out);
    out->tv_sec += millis / 1000L;
    out->tv_nsec += (millis % 1000L) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_nsec -= 1000000000L;
        out->tv_sec += 1;
    }
}

/* Milliseconds left before `deadline`, <= 0 once it has passed. */
static long
millis_until(const struct timespec *deadline) {
    struct timespec moment;
    clock_gettime(CLOCK_MONOTONIC, &moment);
    return (long)(deadline->tv_sec - moment.tv_sec) * 1000L +
           (deadline->tv_nsec - moment.tv_nsec) / 1000000L;
}

/* read_all_fd under a wall-clock deadline.
 *
 * The server reads a new peer's first frame from inside its event loop, so time
 * spent in that read is time nothing else is served - not the pane, not the
 * attached client, not another connection.  A per-read timeout does not fix
 * that on its own, because a peer sending one byte per timeout period resets it
 * forever; the budget has to be absolute and shared across the whole frame,
 * which is what this is.
 *
 * A caller passing NULL gets plain blocking reads.  That is right only for a
 * stream that is already established and is read by its owner at its own pace
 * (kpb_receive).  Every request/reply a client makes of a broker carries a
 * deadline: the broker is another process, it can be stopped or wedged, and an
 * unbounded wait turns one such session into a hang for the caller. */
static ssize_t
read_all_bounded(int fd, void *data, size_t size, const struct timespec *deadline) {
    unsigned char *cursor = data;
    size_t received = 0;
    if (!deadline) return read_all_fd(fd, data, size);
    while (received < size) {
        struct pollfd waiting;
        long remaining = millis_until(deadline);
        ssize_t count;
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        waiting.fd = fd;
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, (int)remaining) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!(waiting.revents & (POLLIN | POLLHUP | POLLERR))) {
            errno = ETIMEDOUT;
            return -1;
        }
        count = read(fd, cursor + received, size - received);
        if (count < 0) {
            /* EAGAIN is possible here despite the poll above: the attached
             * client's socket is non-blocking, and a wakeup can be spurious.
             * Re-poll under the same deadline rather than dropping the peer. */
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        if (count == 0) {
            errno = ECONNRESET;
            return -1;
        }
        received += (size_t)count;
    }
    return (ssize_t)received;
}

static kpb_result
send_frame(int fd, uint16_t type, const void *payload, uint32_t payload_size) {
    kpb_frame_header header;
    struct iovec vectors[2];
    struct msghdr message;
    size_t vector = 0;
    size_t vector_count = payload_size ? 2U : 1U;
    if (payload_size > KPB_PROTOCOL_MAX_PAYLOAD) return KPB_ERR_INVALID;
    header.magic = htonl(KPB_PROTOCOL_MAGIC);
    header.version = htons(KPB_PROTOCOL_VERSION);
    header.type = htons(type);
    header.payload_size = htonl(payload_size);
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof header;
    vectors[1].iov_base = (void *)payload;
    vectors[1].iov_len = payload_size;
    while (vector < vector_count) {
        ssize_t count;
        size_t consumed;
        memset(&message, 0, sizeof message);
        message.msg_iov = vectors + vector;
        message.msg_iovlen = vector_count - vector;
        count = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            return KPB_ERR_SYSTEM;
        }
        if (count == 0) {
            errno = EPIPE;
            return KPB_ERR_SYSTEM;
        }
        consumed = (size_t)count;
        while (vector < vector_count && consumed >= vectors[vector].iov_len) {
            consumed -= vectors[vector].iov_len;
            vector++;
        }
        if (consumed && vector < vector_count) {
            vectors[vector].iov_base =
                (unsigned char *)vectors[vector].iov_base + consumed;
            vectors[vector].iov_len -= consumed;
        }
    }
    return KPB_OK;
}

/* Error payloads always derive their length here.  Hand-typed lengths that
 * disagree with the literal are an over-read, and the existing four were only
 * correct by inspection. */
static kpb_result
send_error(int fd, const char *message) {
    return send_frame(fd, KPB_FRAME_ERROR, message, (uint32_t)strlen(message));
}

static kpb_result
receive_frame_bounded(
    int fd,
    uint16_t *type,
    void *payload,
    size_t capacity,
    uint32_t *payload_size,
    const struct timespec *deadline
) {
    kpb_frame_header header;
    uint32_t size;
    unsigned char discard[4096];
    if (read_all_bounded(fd, &header, sizeof header, deadline) < 0) {
        return errno == ETIMEDOUT ? KPB_ERR_TIMEOUT : KPB_ERR_SYSTEM;
    }
    if (ntohl(header.magic) != KPB_PROTOCOL_MAGIC ||
        ntohs(header.version) != KPB_PROTOCOL_VERSION) {
        return KPB_ERR_PROTOCOL;
    }
    size = ntohl(header.payload_size);
    if (size > KPB_PROTOCOL_MAX_PAYLOAD) return KPB_ERR_PROTOCOL;
    *type = ntohs(header.type);
    *payload_size = size;
    if (size > capacity) {
        uint32_t left = size;
        while (left) {
            size_t chunk = left < sizeof discard ? left : sizeof discard;
            if (read_all_bounded(fd, discard, chunk, deadline) < 0) {
                return errno == ETIMEDOUT ? KPB_ERR_TIMEOUT : KPB_ERR_SYSTEM;
            }
            left -= (uint32_t)chunk;
        }
        return KPB_ERR_BUFFER;
    }
    if (size && read_all_bounded(fd, payload, size, deadline) < 0) {
        return errno == ETIMEDOUT ? KPB_ERR_TIMEOUT : KPB_ERR_SYSTEM;
    }
    return KPB_OK;
}

static bool peer_is_owner(int fd);

/* Connect under `deadline`.  The socket is non-blocking only for the connect:
 * a stopped broker never accept()s, and once its backlog is full a blocking
 * connect() waits for room that never comes, which is precisely the hang the
 * deadline exists to prevent.  A non-blocking connect to a full backlog reports
 * EAGAIN instead, which is retried until the deadline. */
static kpb_result
connect_session_until(
    const char *runtime_dir,
    const char *session_id,
    int *fd_out,
    const struct timespec *deadline
) {
    session_paths paths;
    struct sockaddr_un address;
    int fd;
    int flags;
    kpb_result result = build_paths(runtime_dir, session_id, &paths);
    if (result != KPB_OK) return result;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return KPB_ERR_SYSTEM;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    copy_string(address.sun_path, sizeof address.sun_path, paths.socket_path);
    for (;;) {
        int saved;
        if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0) break;
        saved = errno;
        if (saved == EINTR) continue;
        if (saved == EAGAIN) {
            long remaining = millis_until(deadline);
            if (remaining <= 0) {
                close(fd);
                return KPB_ERR_TIMEOUT;
            }
            (void)poll(NULL, 0, remaining < 10 ? (int)remaining : 10);
            continue;
        }
        close(fd);
        errno = saved;
        return saved == ENOENT || saved == ECONNREFUSED ? KPB_ERR_NOT_FOUND : KPB_ERR_SYSTEM;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return KPB_ERR_SYSTEM;
    }
    /* The server validates clients; the client must validate the server too.
     * Otherwise an explicitly supplied public runtime path could feed terminal
     * controls or forged status data from another uid. */
    if (!peer_is_owner(fd)) {
        close(fd);
        return KPB_ERR_SECURITY;
    }
    *fd_out = fd;
    return KPB_OK;
}

/* A caller's timeout in milliseconds as a deadline; <= 0 is the default. */
static void
deadline_from_timeout(struct timespec *deadline, int timeout_millis, int fallback) {
    deadline_in(deadline, timeout_millis > 0 ? (long)timeout_millis : (long)fallback);
}

static void
status_to_wire(const server_state *server, kpb_wire_status *wire) {
    pid_t foreground = -1;
    memset(wire, 0, sizeof *wire);
    if (server->pty_fd >= 0) foreground = tcgetpgrp(server->pty_fd);
    wire->version = htonl(KPB_PROTOCOL_VERSION);
    wire->broker_pid = (int64_t)host_to_be64((uint64_t)getpid());
    wire->child_pid = (int64_t)host_to_be64((uint64_t)server->child_pid);
    wire->foreground_pgrp = (int64_t)host_to_be64((uint64_t)foreground);
    wire->started_millis = host_to_be64(server->started_millis);
    wire->journal_bytes = host_to_be64(server->journal_bytes);
    wire->journal_epoch = host_to_be64(server->journal_epoch);
    wire->attached = htonl(server->client_fd >= 0 ? 1U : 0U);
    wire->replay_complete = htonl(server->journal_complete ? 1U : 0U);
    wire->rows = htons(server->size.ws_row);
    wire->columns = htons(server->size.ws_col);
    copy_string(wire->session_id, sizeof wire->session_id, server->session_id);
    copy_string(wire->cwd, sizeof wire->cwd, server->cwd);
    copy_string(wire->command, sizeof wire->command, server->command);
}

static kpb_result
wire_to_status(const kpb_wire_status *wire, kpb_status *status) {
    if (ntohl(wire->version) != KPB_PROTOCOL_VERSION) return KPB_ERR_PROTOCOL;
    memset(status, 0, sizeof *status);
    status->broker_pid = (pid_t)(int64_t)be64_to_host((uint64_t)wire->broker_pid);
    status->child_pid = (pid_t)(int64_t)be64_to_host((uint64_t)wire->child_pid);
    status->foreground_pgrp = (pid_t)(int64_t)be64_to_host((uint64_t)wire->foreground_pgrp);
    status->started_millis = be64_to_host(wire->started_millis);
    status->journal_bytes = be64_to_host(wire->journal_bytes);
    status->journal_epoch = be64_to_host(wire->journal_epoch);
    status->attached = ntohl(wire->attached) != 0;
    status->replay_complete = ntohl(wire->replay_complete) != 0;
    status->rows = ntohs(wire->rows);
    status->columns = ntohs(wire->columns);
    if (copy_wire_string(
            status->session_id, sizeof status->session_id,
            wire->session_id, sizeof wire->session_id) != 0 ||
        copy_wire_string(
            status->cwd, sizeof status->cwd,
            wire->cwd, sizeof wire->cwd) != 0 ||
        copy_wire_string(
            status->command, sizeof status->command,
            wire->command, sizeof wire->command) != 0) {
        return KPB_ERR_PROTOCOL;
    }
    return KPB_OK;
}

static int
random_bytes(void *output, size_t size) {
    unsigned char *cursor = output;
    size_t done = 0;
#ifdef __linux__
    while (done < size) {
        ssize_t count = getrandom(cursor + done, size - done, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        done += (size_t)count;
    }
    if (done == size) return 0;
#endif
    {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        if (read_all_fd(fd, cursor + done, size - done) < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        close(fd);
    }
    return 0;
}

void
kpb_spawn_options_init(kpb_spawn_options *options) {
    if (!options) return;
    memset(options, 0, sizeof *options);
    options->journal_limit = KPB_DEFAULT_JOURNAL_LIMIT;
    options->transcript_limit = KPB_DEFAULT_TRANSCRIPT_LIMIT;
    options->transcript_graphics = KPB_TRANSCRIPT_GRAPHICS_ELIDE;
    options->rows = 24;
    options->columns = 80;
}

const char *
kpb_result_string(kpb_result result) {
    switch (result) {
        case KPB_OK: return "success";
        case KPB_ERR_INVALID: return "invalid argument";
        case KPB_ERR_SYSTEM: return "system error";
        case KPB_ERR_SECURITY: return "unsafe path or peer";
        case KPB_ERR_EXISTS: return "session already exists";
        case KPB_ERR_NOT_FOUND: return "session not found";
        case KPB_ERR_BUSY: return "session already attached";
        case KPB_ERR_PROTOCOL: return "protocol error";
        case KPB_ERR_BUFFER: return "buffer too small";
        case KPB_ERR_CHILD: return "child could not start";
        case KPB_ERR_TIMEOUT: return "timed out";
        case KPB_ERR_NAME_TOO_LONG: return "socket path too long";
    }
    return "unknown error";
}

int
kpb_protocol_version(void) {
    return (int)KPB_PROTOCOL_VERSION;
}

kpb_result
kpb_validate_session_id(const char *session_id) {
    return valid_component(session_id) ? KPB_OK : KPB_ERR_INVALID;
}

kpb_result
kpb_generate_session_id(char output[KPB_SESSION_ID_MAX + 1]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char random[16];
    size_t index;
    if (!output) return KPB_ERR_INVALID;
    if (random_bytes(random, sizeof random) != 0) return KPB_ERR_SYSTEM;
    for (index = 0; index < sizeof random; index++) {
        output[index * 2] = hex[random[index] >> 4];
        output[index * 2 + 1] = hex[random[index] & 15];
    }
    output[sizeof random * 2] = '\0';
    return KPB_OK;
}

kpb_result
kpb_prepare_runtime(const char *runtime_dir) {
    session_paths paths;
    kpb_result result;
    if (!runtime_dir || runtime_dir[0] != '/') return KPB_ERR_INVALID;
    result = ensure_private_directory(runtime_dir, true);
    if (result != KPB_OK) return result;
    result = build_paths(runtime_dir, NULL, &paths);
    if (result != KPB_OK) return result;
    return ensure_private_directory(paths.sessions_dir, true);
}

static void
build_command(char output[KPB_COMMAND_MAX], char *const *argv) {
    size_t used = 0;
    size_t index;
    output[0] = '\0';
    for (index = 0; argv && argv[index]; index++) {
        const char *argument = argv[index];
        size_t size = strlen(argument);
        if (index && used + 1 < KPB_COMMAND_MAX) output[used++] = ' ';
        if (used + size >= KPB_COMMAND_MAX) {
            size = KPB_COMMAND_MAX - used - 1;
        }
        memcpy(output + used, argument, size);
        used += size;
        output[used] = '\0';
        if (used + 1 >= KPB_COMMAND_MAX) break;
    }
}

/* Reads at most capacity-1 bytes and NUL-terminates.  The files it is used on
 * are tiny and live in /proc or the session directory. */
static ssize_t
read_small_file(const char *path, char *buffer, size_t capacity) {
    size_t used = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    while (used + 1 < capacity) {
        ssize_t count = read(fd, buffer + used, capacity - 1 - used);
        if (count < 0) {
            int saved = errno;
            if (saved == EINTR) continue;
            close(fd);
            errno = saved;
            return -1;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    close(fd);
    buffer[used] = '\0';
    return (ssize_t)used;
}

/* The kernel's identifier for this boot.  Together with a pid and that
 * process's start time it names one process uniquely across reboots and pid
 * reuse, which a bare pid cannot. */
static int
current_boot_id(char output[64]) {
    char data[96];
    ssize_t size = read_small_file("/proc/sys/kernel/random/boot_id", data, sizeof data);
    size_t index;
    if (size <= 0) return -1;
    while (size > 0 && (data[size - 1] == '\n' || data[size - 1] == ' ')) data[--size] = '\0';
    if (size <= 0 || size >= 64) return -1;
    for (index = 0; index < (size_t)size; index++) {
        char c = data[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F') || c == '-')) {
            return -1;
        }
    }
    memcpy(output, data, (size_t)size + 1);
    return 0;
}

/* Field 22 of /proc/PID/stat: the process's start time in clock ticks since
 * boot.  The command name (field 2) is parenthesised and may itself contain
 * spaces and parentheses, so fields are counted from the LAST ')'. */
static int
process_start_ticks(long pid, uint64_t *ticks) {
    char path[64];
    char data[1024];
    char *cursor;
    char *end = NULL;
    int field;
    unsigned long long value;
    if (pid <= 0 || snprintf(path, sizeof path, "/proc/%ld/stat", pid) >= (int)sizeof path) {
        return -1;
    }
    if (read_small_file(path, data, sizeof data) <= 0) return -1;
    cursor = strrchr(data, ')');
    if (!cursor) return -1;
    cursor++;
    for (field = 3; field < 22; field++) {
        while (*cursor == ' ') cursor++;
        if (!*cursor) return -1;
        while (*cursor && *cursor != ' ') cursor++;
    }
    while (*cursor == ' ') cursor++;
    if (*cursor < '0' || *cursor > '9') return -1;
    errno = 0;
    value = strtoull(cursor, &end, 10);
    if (errno || end == cursor) return -1;
    *ticks = (uint64_t)value;
    return 0;
}

static int
write_metadata(server_state *server) {
    char temporary[KPB_PATH_MAX];
    char data[2048];
    char boot_id[64];
    uint64_t ticks;
    int fd;
    int count;
    int more;
    if (snprintf(temporary, sizeof temporary, "%s.tmp", server->paths.metadata_path) >= (int)sizeof temporary) {
        errno = ENAMETOOLONG;
        return -1;
    }
    count = snprintf(
        data, sizeof data,
        "version=%u\nid=%s\nbroker_pid=%ld\nchild_pid=%ld\nstarted_millis=%llu\n",
        KPB_PROTOCOL_VERSION,
        server->session_id,
        (long)getpid(),
        (long)server->child_pid,
        (unsigned long long)server->started_millis
    );
    if (count < 0 || (size_t)count >= sizeof data) {
        errno = EOVERFLOW;
        return -1;
    }
    /* The identity a later reaper can prove a pid against.  Written only when
     * it can be read: metadata without these fields keeps the pid-only rule. */
    if (current_boot_id(boot_id) == 0) {
        more = snprintf(data + count, sizeof data - (size_t)count, "boot_id=%s\n", boot_id);
        if (more < 0 || (size_t)more >= sizeof data - (size_t)count) {
            errno = EOVERFLOW;
            return -1;
        }
        count += more;
    }
    if (process_start_ticks((long)getpid(), &ticks) == 0) {
        more = snprintf(
            data + count, sizeof data - (size_t)count, "start_ticks=%llu\n",
            (unsigned long long)ticks);
        if (more < 0 || (size_t)more >= sizeof data - (size_t)count) {
            errno = EOVERFLOW;
            return -1;
        }
        count += more;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (write_all_fd(fd, data, (size_t)count) < 0 || fsync(fd) != 0) {
        int saved = errno;
        close(fd);
        unlink(temporary);
        errno = saved;
        return -1;
    }
    if (close(fd) != 0 || rename(temporary, server->paths.metadata_path) != 0) {
        int saved = errno;
        unlink(temporary);
        errno = saved;
        return -1;
    }
    return 0;
}

static int
create_listener(const char *socket_path) {
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    copy_string(address.sun_path, sizeof address.sun_path, socket_path);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
        chmod(socket_path, 0600) != 0 ||
        /* The read-write client, up to KPB_OBSERVER_MAX observers, and
         * concurrent status/list pollers can exceed a backlog of 8 while a
         * frontend is restarting. */
        listen(fd, 16) != 0) {
        int saved = errno;
        close(fd);
        unlink(socket_path);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool
peer_is_owner(int fd) {
#ifdef SO_PEERCRED
    struct ucred credentials;
    socklen_t size = sizeof credentials;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) return false;
    return credentials.uid == geteuid();
#else
    (void)fd;
    return true;
#endif
}

static size_t
frame_queue_pending(const frame_queue *queue) {
    return queue->size - queue->offset;
}

/* Queue one whole frame, or none of it.
 *
 * The capacity check precedes every append: a half-queued frame would
 * permanently desynchronise that peer's framing, which presents as garbled
 * output rather than as an error. */
static int
frame_queue_push(
    frame_queue *queue,
    size_t limit,
    uint16_t type,
    const void *payload,
    uint32_t size
) {
    kpb_frame_header header;
    size_t need = sizeof header + size;
    if (queue->offset && queue->offset == queue->size) {
        queue->offset = queue->size = 0;
    }
    if (queue->size - queue->offset + need > limit) {
        return -1;
    }
    if (queue->offset && queue->size + need > queue->capacity) {
        memmove(
            queue->data,
            queue->data + queue->offset,
            queue->size - queue->offset);
        queue->size -= queue->offset;
        queue->offset = 0;
    }
    if (queue->size + need > queue->capacity) {
        size_t capacity = queue->capacity ? queue->capacity : 65536;
        unsigned char *replacement;
        while (capacity < queue->size + need) capacity *= 2;
        if (capacity > limit) capacity = limit;
        if (queue->size + need > capacity) return -1;
        replacement = realloc(queue->data, capacity);
        if (!replacement) return -1;
        queue->data = replacement;
        queue->capacity = capacity;
    }
    header.magic = htonl(KPB_PROTOCOL_MAGIC);
    header.version = htons(KPB_PROTOCOL_VERSION);
    header.type = htons(type);
    header.payload_size = htonl(size);
    memcpy(queue->data + queue->size, &header, sizeof header);
    if (size) memcpy(queue->data + queue->size + sizeof header, payload, size);
    queue->size += need;
    return 0;
}

static int
frame_queue_flush(int fd, frame_queue *queue) {
    while (queue->offset < queue->size) {
        ssize_t count = send(
            fd,
            queue->data + queue->offset,
            queue->size - queue->offset,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (count == 0) return -1;
        queue->offset += (size_t)count;
    }
    queue->offset = queue->size = 0;
    return 0;
}

static void
frame_queue_release(frame_queue *queue) {
    free(queue->data);
    memset(queue, 0, sizeof *queue);
}

static int
client_enqueue(
    server_state *server,
    uint16_t type,
    const void *payload,
    uint32_t size
) {
    return frame_queue_push(
        &server->client_out, KPB_CLIENT_QUEUE_LIMIT, type, payload, size);
}

static int
client_flush(server_state *server) {
    return frame_queue_flush(server->client_fd, &server->client_out);
}

static void
close_client(server_state *server) {
    if (server->client_fd >= 0) close(server->client_fd);
    server->client_fd = -1;
    frame_queue_release(&server->client_out);
    server->client_replay_active = false;
}

static void
observer_close(server_state *server, size_t slot) {
    observer_slot *observer = &server->observers[slot];
    if (observer->fd >= 0) {
        close(observer->fd);
        if (server->observer_count) server->observer_count--;
    }
    frame_queue_release(&observer->out);
    memset(observer, 0, sizeof *observer);
    observer->fd = -1;
    observer->generation = ++server->observer_generation;
}

static void
observers_close_all(server_state *server) {
    size_t slot;
    for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
        if (server->observers[slot].fd >= 0 || server->observers[slot].out.data) {
            observer_close(server, slot);
        }
    }
}

static int
observer_enqueue(
    observer_slot *observer,
    uint16_t type,
    const void *payload,
    uint32_t size
) {
    return frame_queue_push(
        &observer->out, KPB_OBSERVER_QUEUE_LIMIT, type, payload, size);
}

static int
observer_flush(observer_slot *observer) {
    return frame_queue_flush(observer->fd, &observer->out);
}

/* Refuse an observer with a reason it can read.  The pending queue is dropped
 * first so the short error always fits, then flushed once; a peer that cannot
 * even take that much is simply closed. */
static void
observer_refuse(server_state *server, size_t slot, const char *message) {
    observer_slot *observer = &server->observers[slot];
    observer->out.offset = observer->out.size = 0;
    if (observer_enqueue(
            observer, KPB_FRAME_ERROR, message, (uint32_t)strlen(message)) == 0) {
        (void)observer_flush(observer);
    }
    observer_close(server, slot);
}

static void
observers_send(
    server_state *server,
    uint16_t type,
    const void *payload,
    uint32_t size
) {
    size_t slot;
    for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
        observer_slot *observer = &server->observers[slot];
        if (observer->fd < 0) continue;
        if (observer_enqueue(observer, type, payload, size) != 0 ||
            observer_flush(observer) != 0) {
            observer_close(server, slot);
        }
    }
}

static int
append_journal(server_state *server, const unsigned char *data, size_t size) {
    static const unsigned char reset_sequence[] = "\033c";
    if (server->journal_limit && server->journal_bytes + size > server->journal_limit) {
        size_t keep = size;
        if (ftruncate(server->journal_fd, 0) != 0 ||
            lseek(server->journal_fd, 0, SEEK_SET) < 0 ||
            write_all_fd(server->journal_fd, reset_sequence, sizeof reset_sequence - 1) < 0) {
            return -1;
        }
        server->journal_bytes = sizeof reset_sequence - 1;
        server->journal_epoch++;
        server->journal_complete = false;
        if (server->journal_limit <= server->journal_bytes) {
            if (ftruncate(server->journal_fd, (off_t)server->journal_limit) != 0) {
                return -1;
            }
            server->journal_bytes = server->journal_limit;
            return 0;
        }
        if (keep > server->journal_limit - server->journal_bytes) {
            keep = (size_t)(server->journal_limit - server->journal_bytes);
            data += size - keep;
            size = keep;
        }
    }
    if (write_all_fd(server->journal_fd, data, size) < 0) return -1;
    server->journal_bytes += size;
    return 0;
}

/* Transcript scanner states.  Kitty ships images as APC sequences
 * (ESC _ G ... ESC \) whose payload is base64 pixel data, so a pane running
 * the desktop, a browser, or icat can emit megabytes per second.  Writing that
 * verbatim would evict every readable line from a bounded transcript within
 * seconds, so by default the payload is replaced with a short marker. */
enum {
    TRANSCRIPT_TEXT = 0,
    TRANSCRIPT_ESCAPE,
    TRANSCRIPT_APC,
    TRANSCRIPT_GRAPHICS,
    TRANSCRIPT_GRAPHICS_ESCAPE
};

static ssize_t
pwrite_all_fd(int fd, const void *data, size_t size, off_t offset) {
    const unsigned char *cursor = data;
    size_t written = 0;
    while (written < size) {
        ssize_t count = pwrite(fd, cursor + written, size - written, offset + (off_t)written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) return -1;
        written += (size_t)count;
    }
    return (ssize_t)written;
}

/* The transcript holds raw PTY output, which never says how big the screen
 * was, and a replay cannot place cursor-addressed text without knowing.  The
 * broker therefore records the pane size as a private APC that terminals
 * ignore: when the transcript opens, whenever the size changes, and at the
 * front of the file after a rotation, where ``rotated=1`` also tells a reader
 * that the session's beginning was dropped. */
#define TRANSCRIPT_SIZE_RECORD_MAX 64

static size_t
format_size_record(const server_state *server, bool rotated, unsigned char *out) {
    int length = snprintf(
        (char *)out, TRANSCRIPT_SIZE_RECORD_MAX,
        "\033_kilix-transcript;rows=%u;cols=%u%s\033\\",
        (unsigned)server->size.ws_row, (unsigned)server->size.ws_col,
        rotated ? ";rotated=1" : ""
    );
    if (length <= 0 || length >= TRANSCRIPT_SIZE_RECORD_MAX) return 0;
    return (size_t)length;
}

/* Slide the newest ``keep`` bytes to the front of the transcript and drop the
 * rest.  Rewriting in place (rather than renaming to a .1 file) keeps the
 * single long-lived writer attached to the same inode, matching how the rest
 * of the stack bounds its session logs.  A size record goes first, inside the
 * same budget, because the record that opened the file has just been dropped. */
static int
rotate_transcript(server_state *server, uint64_t keep) {
    unsigned char buffer[KPB_IO_CHUNK];
    unsigned char record[TRANSCRIPT_SIZE_RECORD_MAX];
    size_t record_length = format_size_record(server, true, record);
    off_t read_offset;
    off_t write_offset = 0;
    if (keep > server->transcript_bytes) keep = server->transcript_bytes;
    if (keep >= record_length) keep -= record_length;
    else record_length = 0;
    read_offset = (off_t)(server->transcript_bytes - keep);
    /* The record overwrites only bytes being dropped; with nothing dropped
     * there is no room for it, and nothing was lost to announce. */
    if (read_offset < (off_t)record_length) {
        keep += record_length;
        read_offset -= (off_t)record_length;
        record_length = 0;
    }
    if (record_length) {
        if (pwrite_all_fd(server->transcript_fd, record, record_length, 0) < 0) return -1;
        write_offset = (off_t)record_length;
    }
    while (keep) {
        size_t wanted = keep < sizeof buffer ? (size_t)keep : sizeof buffer;
        ssize_t count = pread(server->transcript_fd, buffer, wanted, read_offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) break;
        if (pwrite_all_fd(server->transcript_fd, buffer, (size_t)count, write_offset) < 0) return -1;
        read_offset += count;
        write_offset += count;
        keep -= (uint64_t)count;
    }
    if (ftruncate(server->transcript_fd, write_offset) != 0) return -1;
    if (lseek(server->transcript_fd, write_offset, SEEK_SET) < 0) return -1;
    server->transcript_bytes = (uint64_t)write_offset;
    return 0;
}

static int
write_transcript(server_state *server, const unsigned char *data, size_t size) {
    if (!size) return 0;
    if (server->transcript_limit && server->transcript_bytes + size > server->transcript_limit) {
        /* Keep three quarters of the budget so a busy pane rotates rarely
         * instead of once per write. */
        uint64_t keep = server->transcript_limit - server->transcript_limit / 4;
        if (size >= keep) {
            unsigned char record[TRANSCRIPT_SIZE_RECORD_MAX];
            size_t record_length = format_size_record(server, true, record);
            if (record_length >= keep) record_length = 0;
            if (ftruncate(server->transcript_fd, 0) != 0 ||
                lseek(server->transcript_fd, 0, SEEK_SET) < 0) {
                return -1;
            }
            if (record_length && write_all_fd(server->transcript_fd, record, record_length) < 0) {
                return -1;
            }
            server->transcript_bytes = record_length;
            data += size - (size_t)(keep - record_length);
            size = (size_t)(keep - record_length);
        } else if (rotate_transcript(server, keep - size) != 0) {
            return -1;
        }
    }
    if (write_all_fd(server->transcript_fd, data, size) < 0) return -1;
    server->transcript_bytes += size;
    return 0;
}

/* Append the current size.  A transcript that cannot be written is closed,
 * as for pane output: the pane keeps running and the log simply ends. */
static void
record_transcript_size(server_state *server) {
    unsigned char record[TRANSCRIPT_SIZE_RECORD_MAX];
    size_t length;
    if (server->transcript_fd < 0) return;
    length = format_size_record(server, false, record);
    if (length && write_transcript(server, record, length) != 0) {
        close(server->transcript_fd);
        server->transcript_fd = -1;
    }
}

#define TRANSCRIPT_MARKER_MAX 96

static size_t
format_elided_graphics(server_state *server, unsigned char *out) {
    int length = snprintf(
        (char *)out, TRANSCRIPT_MARKER_MAX,
        "\r\n[kitty-pty-broker: %llu bytes of graphics elided]\r\n",
        (unsigned long long)server->transcript_elided
    );
    server->transcript_elided = 0;
    if (length <= 0 || length >= TRANSCRIPT_MARKER_MAX) return 0;
    return (size_t)length;
}

static int
reserve_transcript_output(
    server_state *server,
    unsigned char *out,
    size_t capacity,
    size_t *used,
    size_t needed
) {
    if (needed > capacity) {
        errno = EOVERFLOW;
        return -1;
    }
    if (*used > capacity - needed) {
        if (write_transcript(server, out, *used) != 0) return -1;
        *used = 0;
    }
    return 0;
}

/* Copy PTY output into the transcript, dropping kitty graphics payloads.
 *
 * The scanner must survive buffer boundaries, because one APC sequence
 * routinely spans many reads.  The ESC and ESC _ prefixes are therefore held
 * in the scan state rather than written eagerly: by the time the payload is
 * recognised, nothing belonging to the sequence has reached the file yet.  A
 * session killed mid-prefix loses at most those two bytes. */
static int
append_transcript(server_state *server, const unsigned char *data, size_t size) {
    unsigned char out[KPB_IO_CHUNK + TRANSCRIPT_MARKER_MAX * 2];
    size_t out_length = 0;
    size_t index = 0;
    if (server->transcript_fd < 0 || !size) return 0;
    if (server->transcript_graphics == KPB_TRANSCRIPT_GRAPHICS_KEEP) {
        return write_transcript(server, data, size);
    }
    while (index < size) {
        unsigned char byte;
        switch (server->transcript_scan) {
            case TRANSCRIPT_TEXT: {
                const unsigned char *escape = memchr(
                    data + index, 0x1b, size - index);
                size_t run = escape
                    ? (size_t)(escape - (data + index)) : size - index;
                if (reserve_transcript_output(
                        server, out, sizeof out, &out_length, run) != 0) {
                    return -1;
                }
                memcpy(out + out_length, data + index, run);
                out_length += run;
                index += run;
                if (index < size) {
                    server->transcript_scan = TRANSCRIPT_ESCAPE;
                    index++;
                }
                break;
            }
            case TRANSCRIPT_ESCAPE:
                byte = data[index++];
                if (byte == '_') {
                    server->transcript_scan = TRANSCRIPT_APC;
                    break;
                }
                if (reserve_transcript_output(
                        server, out, sizeof out, &out_length, 2) != 0) {
                    return -1;
                }
                out[out_length++] = 0x1b;
                if (byte == 0x1b) {
                    server->transcript_scan = TRANSCRIPT_ESCAPE;
                } else {
                    out[out_length++] = byte;
                    server->transcript_scan = TRANSCRIPT_TEXT;
                }
                break;
            case TRANSCRIPT_APC:
                byte = data[index++];
                if (byte == 'G') {
                    server->transcript_scan = TRANSCRIPT_GRAPHICS;
                    server->transcript_elided = 3;
                    break;
                }
                if (reserve_transcript_output(
                        server, out, sizeof out, &out_length, 3) != 0) {
                    return -1;
                }
                out[out_length++] = 0x1b;
                out[out_length++] = '_';
                if (byte == 0x1b) {
                    server->transcript_scan = TRANSCRIPT_ESCAPE;
                } else {
                    out[out_length++] = byte;
                    server->transcript_scan = TRANSCRIPT_TEXT;
                }
                break;
            case TRANSCRIPT_GRAPHICS: {
                const unsigned char *escape = memchr(
                    data + index, 0x1b, size - index);
                const unsigned char *bell = memchr(
                    data + index, 0x07, size - index);
                const unsigned char *special = !escape ? bell :
                    (!bell || escape < bell ? escape : bell);
                size_t run = special
                    ? (size_t)(special - (data + index)) : size - index;
                server->transcript_elided += (uint64_t)run;
                index += run;
                if (index == size) break;
                byte = data[index++];
                server->transcript_elided++;
                if (byte == 0x1b) {
                    server->transcript_scan = TRANSCRIPT_GRAPHICS_ESCAPE;
                } else if (byte == 0x07) {
                    server->transcript_scan = TRANSCRIPT_TEXT;
                    if (reserve_transcript_output(
                            server, out, sizeof out, &out_length,
                            TRANSCRIPT_MARKER_MAX) != 0) {
                        return -1;
                    }
                    out_length += format_elided_graphics(server, out + out_length);
                }
                break;
            }
            case TRANSCRIPT_GRAPHICS_ESCAPE:
                byte = data[index++];
                server->transcript_elided++;
                if (byte == '\\') {
                    server->transcript_scan = TRANSCRIPT_TEXT;
                    if (reserve_transcript_output(
                            server, out, sizeof out, &out_length,
                            TRANSCRIPT_MARKER_MAX) != 0) {
                        return -1;
                    }
                    out_length += format_elided_graphics(server, out + out_length);
                } else if (byte != 0x1b) {
                    server->transcript_scan = TRANSCRIPT_GRAPHICS;
                }
                break;
            default:
                server->transcript_scan = TRANSCRIPT_TEXT;
                byte = data[index++];
                if (reserve_transcript_output(
                        server, out, sizeof out, &out_length, 1) != 0) {
                    return -1;
                }
                out[out_length++] = byte;
                break;
        }
    }
    if (out_length) return write_transcript(server, out, out_length);
    return 0;
}

/* What a newly admitted peer is owed.  `end` is snapshotted before streaming
 * begins: that snapshot is what keeps replayed history and the first live
 * bytes contiguous, with neither a gap nor a duplicate. */
typedef struct {
    uint64_t start;
    uint64_t end;
    bool resumed;
    bool truncated;
} replay_plan;

/* Replay history to a freshly admitted observer.  Reads use pread so the
 * journal's own append offset is never disturbed, and clamp to plan->end
 * rather than the file size: when journal_limit is smaller than the reset
 * sequence, journal_bytes deliberately exceeds the file.  Observer-only,
 * because an observer's replay is capped and therefore always fits its queue;
 * the read-write client's replay has no such cap and is fed incrementally
 * from the event loop instead. */
static int
replay_journal(
    server_state *server,
    observer_slot *observer,
    const replay_plan *plan
) {
    unsigned char buffer[KPB_IO_CHUNK];
    uint64_t at = plan->start;
    /* Sent as its own frame type, not as OUTPUT: these two bytes are not in
     * the journal, and a peer that counted them as journal content would carry
     * a two-byte error in its resume offset for the life of the session. */
    if (plan->truncated &&
        observer_enqueue(observer, KPB_FRAME_RESET, "\033c", 2) != 0) {
        return -1;
    }
    while (at < plan->end) {
        uint64_t left = plan->end - at;
        size_t wanted = left < sizeof buffer ? (size_t)left : sizeof buffer;
        ssize_t count = pread(server->journal_fd, buffer, wanted, (off_t)at);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) break;
        if (observer_enqueue(
                observer, KPB_FRAME_OUTPUT, buffer, (uint32_t)count) != 0) {
            return -1;
        }
        at += (uint64_t)count;
    }
    return observer_enqueue(observer, KPB_FRAME_REPLAY_DONE, NULL, 0);
}

/* Begin owing the read-write client a replay.  The frames are fed to its
 * queue from the event loop rather than streamed here: the journal can hold
 * tens of megabytes, and sending it synchronously was the write-side hole in
 * the loop's responsiveness guarantee - a client that attached and never read
 * held the broker, and therefore `kill`, hostage inside sendmsg.  Feeding
 * from the loop keeps memory bounded by the queue limit and the control
 * plane live throughout. */
static void
client_replay_begin(server_state *server, uint64_t start) {
    server->client_replay_active = true;
    server->client_replay_at = start;
    server->client_replay_epoch = server->journal_epoch;
}

/* Feed the pending replay up to the queue's high-water mark.  The end is
 * journal_bytes rather than a snapshot: within an epoch the journal is
 * append-only, so output arriving while the replay drains is simply covered
 * by the replay itself, and REPLAY_DONE is sent exactly when the client is
 * current.  If the epoch rolls over mid-replay the journal now begins with a
 * terminal reset, so restarting from zero repaints the client correctly -
 * the same contract every full replay after eviction has. */
static void
feed_client_replay(server_state *server) {
    unsigned char buffer[KPB_IO_CHUNK];
    while (server->client_fd >= 0 && server->client_replay_active &&
           frame_queue_pending(&server->client_out) < KPB_CLIENT_QUEUE_HIGH_WATER) {
        uint64_t end = server->journal_bytes;
        size_t wanted;
        ssize_t count;
        if (server->client_replay_epoch != server->journal_epoch) {
            server->client_replay_epoch = server->journal_epoch;
            server->client_replay_at = 0;
            continue;
        }
        if (server->client_replay_at >= end) {
            if (client_enqueue(server, KPB_FRAME_REPLAY_DONE, NULL, 0) != 0) {
                close_client(server);
                return;
            }
            server->client_replay_active = false;
            return;
        }
        wanted = end - server->client_replay_at < sizeof buffer
            ? (size_t)(end - server->client_replay_at) : sizeof buffer;
        count = pread(
            server->journal_fd, buffer, wanted, (off_t)server->client_replay_at);
        if (count < 0) {
            if (errno == EINTR) continue;
            close_client(server);
            return;
        }
        if (count == 0) {
            /* journal_bytes deliberately exceeds the file when the limit is
             * smaller than the reset sequence; end-of-file means caught up,
             * exactly as it did for the streaming replay. */
            server->client_replay_at = end;
            continue;
        }
        if (client_enqueue(
                server, KPB_FRAME_OUTPUT, buffer, (uint32_t)count) != 0) {
            close_client(server);
            return;
        }
        server->client_replay_at += (uint64_t)count;
    }
}

/* Decide what a peer is owed.  Within one epoch the journal is strictly
 * append-only, so an offset only ever becomes invalid by the epoch rolling
 * over - which is exactly what eviction is here.  Anything unusable silently
 * becomes a full replay. */
static void
plan_replay(
    const server_state *server,
    const kpb_wire_attach *request,
    bool observer,
    replay_plan *plan
) {
    plan->start = 0;
    plan->end = server->journal_bytes;
    plan->resumed = false;
    plan->truncated = false;
    if ((ntohl(request->flags) & KPB_ATTACH_FLAG_RESUME) &&
        be64_to_host(request->resume_epoch) == server->journal_epoch &&
        be64_to_host(request->resume_offset) <= server->journal_bytes) {
        plan->start = be64_to_host(request->resume_offset);
        plan->resumed = true;
    }
    if (observer && plan->end - plan->start > KPB_OBSERVER_REPLAY_MAX) {
        plan->start = plan->end - KPB_OBSERVER_REPLAY_MAX;
        plan->resumed = false;
        plan->truncated = true;
    }
}

static void
apply_size(server_state *server, const kpb_wire_winsize *wire) {
    unsigned short rows = server->size.ws_row;
    unsigned short columns = server->size.ws_col;
    server->size.ws_row = ntohs(wire->rows);
    server->size.ws_col = ntohs(wire->columns);
    server->size.ws_xpixel = ntohs(wire->xpixel);
    server->size.ws_ypixel = ntohs(wire->ypixel);
    if (!server->size.ws_row) server->size.ws_row = 24;
    if (!server->size.ws_col) server->size.ws_col = 80;
    if (server->pty_fd >= 0) (void)ioctl(server->pty_fd, TIOCSWINSZ, &server->size);
    if (rows != server->size.ws_row || columns != server->size.ws_col) {
        record_transcript_size(server);
    }
}

static void
signal_child_session(pid_t session_id, int signal_number) {
    DIR *directory;
    struct dirent *entry;
    if (session_id <= 0) return;
    directory = opendir("/proc");
    if (directory) {
        while ((entry = readdir(directory))) {
            char *end = NULL;
            long value;
            errno = 0;
            value = strtol(entry->d_name, &end, 10);
            if (errno || !end || *end || value <= 0 || value > INT32_MAX) continue;
            if (getsid((pid_t)value) == session_id) {
                (void)kill((pid_t)value, signal_number);
            }
        }
        closedir(directory);
    }
    (void)killpg(session_id, signal_number);
}

static void
request_termination(server_state *server) {
    if (server->terminate_requested) return;
    server->terminate_requested = true;
    server->terminate_deadline = monotonic_millis() + 1500;
    signal_child_session(server->child_pid, SIGTERM);
}

static int
queue_input(server_state *server, const unsigned char *data, size_t size) {
    size_t needed;
    if (!size) return 0;
    if (size > KPB_INPUT_BUFFER_LIMIT - server->input_size) {
        errno = ENOBUFS;
        return -1;
    }
    if (server->input_offset &&
        server->input_offset + server->input_size + size > server->input_capacity) {
        memmove(
            server->input_buffer,
            server->input_buffer + server->input_offset,
            server->input_size);
        server->input_offset = 0;
    }
    needed = server->input_offset + server->input_size + size;
    if (needed > server->input_capacity) {
        size_t capacity = server->input_capacity ? server->input_capacity : 65536;
        unsigned char *replacement;
        while (capacity < needed) capacity *= 2;
        replacement = realloc(server->input_buffer, capacity);
        if (!replacement) return -1;
        server->input_buffer = replacement;
        server->input_capacity = capacity;
    }
    memcpy(
        server->input_buffer + server->input_offset + server->input_size,
        data, size);
    server->input_size += size;
    return 0;
}

static int
flush_input(server_state *server) {
    while (server->input_size) {
        ssize_t written = write(
            server->pty_fd,
            server->input_buffer + server->input_offset,
            server->input_size);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (written == 0) return 0;
        server->input_offset += (size_t)written;
        server->input_size -= (size_t)written;
    }
    server->input_offset = 0;
    return 0;
}

static int
set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
handle_v1_attach(server_state *server, int fd, const unsigned char *payload) {
    if (server->client_fd >= 0) {
        (void)send_error(fd, KPB_ERROR_ATTACHED);
        close(fd);
        return;
    }
    if (set_nonblocking(fd) != 0) {
        close(fd);
        return;
    }
    server->client_fd = fd;
    client_replay_begin(server, 0);
    {
        /* memcpy rather than a cast: the receive buffer is an unsigned char
         * array with alignment 1. */
        kpb_wire_winsize size;
        memcpy(&size, payload, sizeof size);
        apply_size(server, &size);
    }
}

static void
refuse_v2(int fd, kpb_result code) {
    kpb_wire_attach_reply reply;
    memset(&reply, 0, sizeof reply);
    reply.result = htons((uint16_t)code);
    reply.version = htons((uint16_t)KPB_PROTOCOL_VERSION_MAX);
    (void)send_frame(fd, KPB_FRAME_ATTACH_REPLY, &reply, sizeof reply);
    close(fd);
}

/* Admit a session-protocol-2 peer.  Reached only for a 32-byte payload, which
 * is what guarantees a v1 peer never sees the reply frame. */
static void
handle_v2_handshake(
    server_state *server,
    int fd,
    uint16_t type,
    const kpb_wire_attach *request
) {
    kpb_wire_attach_reply reply;
    replay_plan plan;
    uint16_t selected = ntohs(request->version);
    uint16_t mode = ntohs(request->mode);
    uint32_t request_flags = ntohl(request->flags);
    bool observing = type == KPB_FRAME_OBSERVE;

    if (selected > KPB_PROTOCOL_VERSION_MAX) {
        selected = (uint16_t)KPB_PROTOCOL_VERSION_MAX;
    }
    /* A 32-byte request that declares it cannot speak version 2 contradicts
     * itself.  The refusal still reports the true ceiling so the peer can
     * retry correctly. */
    if (selected < 2) {
        refuse_v2(fd, KPB_ERR_PROTOCOL);
        return;
    }
    if (mode != (observing ? KPB_WIRE_MODE_OBSERVE : KPB_WIRE_MODE_CONTROL)) {
        refuse_v2(fd, KPB_ERR_PROTOCOL);
        return;
    }
    if (request_flags & ~KPB_ATTACH_FLAG_RESUME) {
        refuse_v2(fd, KPB_ERR_PROTOCOL);
        return;
    }

    plan_replay(server, request, observing, &plan);
    memset(&reply, 0, sizeof reply);
    reply.version = htons(selected);
    reply.result = 0;
    reply.journal_epoch = host_to_be64(server->journal_epoch);
    reply.journal_offset = host_to_be64(plan.start);
    reply.flags = htonl(
        (plan.resumed ? KPB_REPLY_FLAG_RESUMED : 0U) |
        (server->journal_complete ? KPB_REPLY_FLAG_COMPLETE : 0U) |
        (plan.truncated ? KPB_REPLY_FLAG_TRUNCATED : 0U));

    if (!observing) {
        if (server->client_fd >= 0) {
            refuse_v2(fd, KPB_ERR_BUSY);
            return;
        }
        if (set_nonblocking(fd) != 0) {
            refuse_v2(fd, KPB_ERR_SYSTEM);
            return;
        }
        server->client_fd = fd;
        if (client_enqueue(
                server, KPB_FRAME_ATTACH_REPLY, &reply, sizeof reply) != 0) {
            close_client(server);
            return;
        }
        client_replay_begin(server, plan.start);
        {
            kpb_wire_winsize size;
            size.rows = request->rows;
            size.columns = request->columns;
            size.xpixel = request->xpixel;
            size.ypixel = request->ypixel;
            apply_size(server, &size);
        }
        return;
    }

    {
        size_t slot;
        observer_slot *observer;
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            if (server->observers[slot].fd < 0) break;
        }
        if (slot == KPB_OBSERVER_MAX) {
            refuse_v2(fd, KPB_ERR_BUSY);
            return;
        }
        if (set_nonblocking(fd) != 0) {
            refuse_v2(fd, KPB_ERR_SYSTEM);
            return;
        }
        observer = &server->observers[slot];
        frame_queue_release(&observer->out);
        memset(observer, 0, sizeof *observer);
        observer->fd = fd;
        observer->generation = ++server->observer_generation;
        server->observer_count++;
        /* Never apply_size here: an observer must not be able to resize the
         * pane, and the v1 path above calls apply_size unconditionally. */
        if (observer_enqueue(
                observer, KPB_FRAME_ATTACH_REPLY, &reply, sizeof reply) != 0 ||
            replay_journal(server, observer, &plan) != 0 ||
            observer_flush(observer) != 0) {
            observer_close(server, slot);
        }
    }
}

static void
handle_new_connection(server_state *server) {
    unsigned char payload[sizeof(kpb_wire_status)];
    struct timespec deadline;
    uint32_t payload_size = 0;
    uint16_t type = 0;
    int fd = accept4(server->listener_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    if (!peer_is_owner(fd)) {
        (void)send_error(fd, KPB_ERROR_UNAUTHORIZED);
        close(fd);
        return;
    }
    /* Half a second.  A local peer writes its first frame immediately after
     * connect, so this is generous by orders of magnitude, and it is kept short
     * deliberately: the budget is also the longest the loop can be held by one
     * connection, so it is the number that bounds how much a same-user peer can
     * degrade responsiveness by connecting and saying nothing.  It cannot stop
     * the broker, which is what the old unbounded read allowed; it can still
     * slow it, which a non-blocking handshake driven from the loop would fix
     * and this does not. */
    deadline_in(&deadline, 500);
    if (receive_frame_bounded(
            fd, &type, payload, sizeof payload, &payload_size, &deadline) != KPB_OK) {
        close(fd);
        return;
    }
    if (type == KPB_FRAME_STATUS && payload_size == 0) {
        kpb_wire_status status;
        status_to_wire(server, &status);
        (void)send_frame(fd, KPB_FRAME_STATUS_REPLY, &status, sizeof status);
        close(fd);
        return;
    }
    if (type == KPB_FRAME_TERMINATE && payload_size == 0) {
        (void)send_frame(fd, KPB_FRAME_ACK, NULL, 0);
        close(fd);
        request_termination(server);
        return;
    }
    /* The v1 branch is checked first and its body is unchanged.  That ordering
     * IS the compatibility guarantee: a peer that presented an 8-byte attach
     * can never reach code that emits a frame type it does not parse. */
    if (type == KPB_FRAME_ATTACH && payload_size == sizeof(kpb_wire_winsize)) {
        handle_v1_attach(server, fd, payload);
        return;
    }
    if ((type == KPB_FRAME_ATTACH || type == KPB_FRAME_OBSERVE) &&
        payload_size == sizeof(kpb_wire_attach)) {
        kpb_wire_attach request;
        memcpy(&request, payload, sizeof request);
        handle_v2_handshake(server, fd, type, &request);
        return;
    }
    if (type == KPB_FRAME_OBSERVE) {
        (void)send_error(fd, KPB_ERROR_NEEDS_V2);
        close(fd);
        return;
    }
    (void)send_error(fd, KPB_ERROR_INVALID);
    close(fd);
}

static void
handle_client_frame(server_state *server) {
    unsigned char payload[KPB_IO_CHUNK];
    struct timespec deadline;
    uint32_t payload_size = 0;
    uint16_t type = 0;
    kpb_result result;

    /* Bounded for the same reason the accept path is, and it was a mistake to
     * bound only that one: an attached client's frames are read from the same
     * event loop, so a peer that stops mid-frame stops the broker just as
     * completely one frame later.  Measured on the version that fixed only the
     * accept path - a client sending one byte of a header held the loop
     * indefinitely, and `kill` could not recover it.
     *
     * Two seconds rather than the accept path's half, because this peer is
     * legitimate and may be briefly descheduled or backpressured while
     * transmitting a maximum-sized frame. */
    deadline_in(&deadline, 2000);
    result = receive_frame_bounded(
        server->client_fd, &type, payload, sizeof payload, &payload_size,
        &deadline
    );
    if (result != KPB_OK) {
        close_client(server);
        return;
    }
    switch (type) {
        case KPB_FRAME_INPUT:
            if (queue_input(server, payload, payload_size) != 0) {
                /* Refuse with a reason it can read, the way observer_refuse
                 * does: drop the pending queue so the short error always
                 * fits, flush once, and close. */
                server->client_out.offset = server->client_out.size = 0;
                if (client_enqueue(
                        server, KPB_FRAME_ERROR, KPB_ERROR_INPUT_LIMIT,
                        (uint32_t)strlen(KPB_ERROR_INPUT_LIMIT)) == 0) {
                    (void)client_flush(server);
                }
                close_client(server);
            }
            break;
        case KPB_FRAME_RESIZE:
            if (payload_size == sizeof(kpb_wire_winsize)) {
                /* The receive buffer is an unsigned-char array and therefore
                 * promises no alignment suitable for kpb_wire_winsize. */
                kpb_wire_winsize size;
                memcpy(&size, payload, sizeof size);
                apply_size(server, &size);
            } else {
                close_client(server);
            }
            break;
        case KPB_FRAME_DETACH:
            close_client(server);
            break;
        default:
            close_client(server);
            break;
    }
}

/* Every inbound frame from an observer ends the connection, so only the header
 * is ever read.  A hostile observer therefore cannot make the broker read an
 * attacker-chosen payload_size, nor stall it on a partial payload. */
static void
handle_observer_frame(server_state *server, size_t slot) {
    observer_slot *observer = &server->observers[slot];
    kpb_frame_header header;
    ssize_t count = recv(
        observer->fd,
        observer->in + observer->in_size,
        sizeof observer->in - observer->in_size,
        MSG_DONTWAIT);
    if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        observer_close(server, slot);
        return;
    }
    if (count == 0) {
        observer_close(server, slot);
        return;
    }
    observer->in_size += (size_t)count;
    if (observer->in_size < sizeof observer->in) return;
    memcpy(&header, observer->in, sizeof header);
    observer->in_size = 0;
    if (ntohl(header.magic) != KPB_PROTOCOL_MAGIC ||
        ntohs(header.version) != KPB_PROTOCOL_VERSION) {
        observer_close(server, slot);
        return;
    }
    switch (ntohs(header.type)) {
        case KPB_FRAME_DETACH:
            observer_close(server, slot);
            break;
        case KPB_FRAME_INPUT:
        case KPB_FRAME_RESIZE:
            observer_refuse(server, slot, KPB_ERROR_READ_ONLY);
            break;
        default:
            observer_refuse(server, slot, KPB_ERROR_INVALID);
            break;
    }
}

static int
forward_pty_output(server_state *server, const unsigned char *data, size_t size) {
    if (append_journal(server, data, size) != 0) return -1;
    /* A transcript is best-effort: a full disk or a revoked directory must
     * not take down a live shell. */
    if (append_transcript(server, data, size) != 0) {
        close(server->transcript_fd);
        server->transcript_fd = -1;
    }
    /* During a replay these bytes are already covered by the replay itself,
     * which reads them back out of the journal; queueing them here too would
     * deliver them twice. */
    if (server->client_fd >= 0 && !server->client_replay_active &&
        (client_enqueue(server, KPB_FRAME_OUTPUT, data, (uint32_t)size) != 0 ||
         client_flush(server) != 0)) {
        close_client(server);
    }
    observers_send(server, KPB_FRAME_OUTPUT, data, (uint32_t)size);
    return 0;
}

/* Consume output the child wrote just before exiting.
 *
 * Reaping the child does not empty the PTY: bytes written immediately before
 * exit are still buffered, and the main loop would discard them on its next
 * condition check.  Those are the most valuable bytes in the stream — the
 * error a pane printed on its way out — so they are drained here into the
 * journal, the transcript, and any attached client.  Bounded, because a
 * surviving grandchild can hold the slave open and keep writing forever. */
static void
drain_pty(server_state *server) {
    unsigned char buffer[KPB_IO_CHUNK];
    const uint64_t deadline = monotonic_millis() + 200;
    while (monotonic_millis() < deadline) {
        struct pollfd descriptor = {.fd = server->pty_fd, .events = POLLIN, .revents = 0};
        size_t used = 0;
        int ready = poll(&descriptor, 1, 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) return;
        while (used < sizeof buffer) {
            ssize_t count = read(server->pty_fd, buffer + used, sizeof buffer - used);
            if (count > 0) {
                used += (size_t)count;
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
        if (!used || forward_pty_output(server, buffer, used) != 0) return;
    }
}

/* Give observers a bounded chance to receive what is already queued, on the
 * same budget drain_pty uses.  A peer that will not read within it is closed
 * rather than allowed to delay teardown. */
static void
observers_drain(server_state *server) {
    const uint64_t deadline = monotonic_millis() + 200;
    while (monotonic_millis() < deadline) {
        struct pollfd descriptors[KPB_OBSERVER_MAX];
        size_t slots[KPB_OBSERVER_MAX];
        nfds_t pending = 0;
        size_t slot;
        int ready;
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            observer_slot *observer = &server->observers[slot];
            if (observer->fd < 0 || !frame_queue_pending(&observer->out)) continue;
            descriptors[pending].fd = observer->fd;
            descriptors[pending].events = POLLOUT;
            descriptors[pending].revents = 0;
            slots[pending] = slot;
            pending++;
        }
        if (!pending) return;
        ready = poll(descriptors, pending, 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) continue;
        for (slot = 0; slot < (size_t)pending; slot++) {
            if (!descriptors[slot].revents) continue;
            if (observer_flush(&server->observers[slots[slot]]) != 0) {
                observer_close(server, slots[slot]);
            }
        }
    }
}

/* Give the read-write client a bounded chance to take what it is owed - the
 * tail of its replay, any queued output, and the EXIT frame.
 *
 * The bound is idle time, not total time: a client that attached just before
 * the child exited can still be owed most of a large journal, and delivering
 * that takes however long the client takes to read it, so a single fixed
 * budget would cut a well-behaved slow reader off mid-replay and cost it the
 * EXIT frame - and with it the child's real exit status.  Every delivery
 * therefore resets the deadline, and only a client that takes nothing for a
 * whole budget has stopped behaving and is closed rather than allowed to
 * hold teardown open, which is what an unbounded send here once permitted: a
 * stalled frontend made `kill` hang forever on a session whose child was
 * already gone.  The overall cap keeps teardown finite even against a client
 * that trickles one byte per budget. */
#define KPB_FINISH_IDLE_MILLIS 200
#define KPB_FINISH_TOTAL_MILLIS 30000

static void
client_finish(server_state *server, const kpb_wire_exit *wire) {
    const uint64_t abandon = monotonic_millis() + KPB_FINISH_TOTAL_MILLIS;
    uint64_t stalled = monotonic_millis() + KPB_FINISH_IDLE_MILLIS;
    bool exit_queued = false;
    while (server->client_fd >= 0 && monotonic_millis() < stalled &&
           monotonic_millis() < abandon) {
        struct pollfd descriptor;
        size_t pending_before;
        feed_client_replay(server);
        if (server->client_fd < 0) return;
        if (!server->client_replay_active && !exit_queued) {
            if (client_enqueue(server, KPB_FRAME_EXIT, wire, sizeof *wire) != 0) {
                close_client(server);
                return;
            }
            exit_queued = true;
        }
        pending_before = frame_queue_pending(&server->client_out);
        if (client_flush(server) != 0) {
            close_client(server);
            return;
        }
        if (frame_queue_pending(&server->client_out) < pending_before) {
            stalled = monotonic_millis() + KPB_FINISH_IDLE_MILLIS;
        }
        if (exit_queued && !frame_queue_pending(&server->client_out)) return;
        descriptor.fd = server->client_fd;
        descriptor.events = POLLOUT;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 20) < 0 && errno != EINTR) return;
    }
}

static int
server_loop(server_state *server) {
    unsigned char buffer[KPB_IO_CHUNK];
    int child_status = 0;
    bool child_exited = false;
    while (!child_exited) {
        struct pollfd descriptors[3 + KPB_OBSERVER_MAX];
        struct timespec timeout;
        struct timespec *timeout_pointer = NULL;
        sigset_t wait_mask;
        uint32_t generations[KPB_OBSERVER_MAX];
        size_t slot;
        int result;
        pid_t waited = waitpid(server->child_pid, &child_status, WNOHANG);
        if (waited == server->child_pid) child_exited = true;
        else if (waited < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (server->terminate_requested && monotonic_millis() >= server->terminate_deadline) {
            signal_child_session(server->child_pid, SIGKILL);
            server->terminate_deadline = UINT64_MAX;
        }
        feed_client_replay(server);
        descriptors[0].fd = server->listener_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = server->pty_fd;
        /* Reading the PTY is gated on the client's backlog: past the
         * high-water mark the kernel PTY buffer is left to fill, which is
         * what stops the shell when a frontend stops reading.  Everything
         * else - the listener, observers, the client's own frames - stays
         * serviced, so `status` and `kill` keep working regardless. */
        descriptors[1].events = (short)(
            (server->client_fd >= 0 &&
             frame_queue_pending(&server->client_out) >=
                 KPB_CLIENT_QUEUE_HIGH_WATER
                 ? 0 : POLLIN) |
            (server->input_size ? POLLOUT : 0));
        descriptors[1].revents = 0;
        descriptors[2].fd = server->client_fd;
        descriptors[2].events = server->client_fd >= 0
            ? (short)(POLLIN |
                (frame_queue_pending(&server->client_out) ? POLLOUT : 0))
            : 0;
        descriptors[2].revents = 0;
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            observer_slot *observer = &server->observers[slot];
            descriptors[3 + slot].fd = observer->fd;
            descriptors[3 + slot].events = observer->fd >= 0
                ? (short)(POLLIN |
                    (frame_queue_pending(&observer->out) ? POLLOUT : 0))
                : 0;
            descriptors[3 + slot].revents = 0;
            generations[slot] = observer->generation;
        }
        if (child_exited) {
            timeout.tv_sec = 0;
            timeout.tv_nsec = 0;
            timeout_pointer = &timeout;
        } else if (server->terminate_requested &&
                   server->terminate_deadline != UINT64_MAX) {
            uint64_t now = monotonic_millis();
            uint64_t remaining = server->terminate_deadline > now
                ? server->terminate_deadline - now : 0;
            timeout.tv_sec = (time_t)(remaining / 1000U);
            timeout.tv_nsec = (long)(remaining % 1000U) * 1000000L;
            timeout_pointer = &timeout;
        }
        sigemptyset(&wait_mask);
        result = ppoll(
            descriptors, 3 + KPB_OBSERVER_MAX, timeout_pointer, &wait_mask);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (descriptors[0].revents & POLLIN) handle_new_connection(server);
        if (server->client_fd >= 0 && (descriptors[2].revents & POLLOUT)) {
            if (client_flush(server) != 0) close_client(server);
        }
        if (server->client_fd >= 0 &&
            (descriptors[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (descriptors[2].revents & POLLIN) handle_client_frame(server);
            else close_client(server);
        }
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            observer_slot *observer = &server->observers[slot];
            short revents = descriptors[3 + slot].revents;
            /* The accept step above may have recycled this slot since poll
             * returned, so revalidate identity before acting on it. */
            if (observer->fd < 0 ||
                descriptors[3 + slot].fd != observer->fd ||
                observer->generation != generations[slot]) {
                continue;
            }
            if (revents & POLLOUT) {
                if (observer_flush(observer) != 0) {
                    observer_close(server, slot);
                    continue;
                }
            }
            if (revents & POLLIN) handle_observer_frame(server, slot);
            else if (revents & (POLLHUP | POLLERR)) observer_close(server, slot);
        }
        if (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            size_t used = 0;
            while (used < sizeof buffer) {
                ssize_t count = read(
                    server->pty_fd, buffer + used, sizeof buffer - used);
                if (count > 0) {
                    used += (size_t)count;
                    continue;
                }
                if (count < 0 && errno == EINTR) continue;
                break;
            }
            if (used && forward_pty_output(server, buffer, used) != 0) return -1;
        }
        if (descriptors[1].revents & POLLOUT) {
            if (flush_input(server) != 0) return -1;
        }
        /* Bound backlog latency to one tick.  This cannot spin: a queue that
         * survives the flush means the socket is full, and POLLOUT then simply
         * does not fire until it drains. */
        if (server->client_fd >= 0 && frame_queue_pending(&server->client_out)) {
            if (client_flush(server) != 0) close_client(server);
        }
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            observer_slot *observer = &server->observers[slot];
            if (observer->fd < 0 || !frame_queue_pending(&observer->out)) continue;
            if (observer_flush(observer) != 0) observer_close(server, slot);
        }
    }
    drain_pty(server);
    {
        kpb_wire_exit wire;
        wire.wait_status = htonl((uint32_t)child_status);
        client_finish(server, &wire);
        observers_send(server, KPB_FRAME_EXIT, &wire, sizeof wire);
        observers_drain(server);
    }
    close_client(server);
    observers_close_all(server);
    return wait_status_to_exit_code(child_status);
}

static void
cleanup_server(server_state *server) {
    observers_close_all(server);
    if (server->client_fd >= 0) close(server->client_fd);
    frame_queue_release(&server->client_out);
    if (server->pty_fd >= 0) close(server->pty_fd);
    if (server->listener_fd >= 0) close(server->listener_fd);
    if (server->journal_fd >= 0) close(server->journal_fd);
    if (server->transcript_fd >= 0) close(server->transcript_fd);
    free(server->input_buffer);
    unlink(server->paths.socket_path);
    unlink(server->paths.journal_path);
    unlink(server->paths.metadata_path);
    rmdir(server->paths.session_dir);
}

static int
reset_child_signals(void) {
    struct sigaction action;
    sigset_t mask;
    int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCHLD, SIGPIPE};
    size_t index;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (index = 0; index < sizeof signals / sizeof signals[0]; index++) {
        if (sigaction(signals[index], &action, NULL) != 0) return -1;
    }
    sigemptyset(&mask);
    return sigprocmask(SIG_SETMASK, &mask, NULL);
}

static void
handle_server_child(int signal_number) {
    (void)signal_number;
}

static int
configure_server_signals(void) {
    struct sigaction action;
    sigset_t mask;
    int ignored[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE};
    size_t index;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    /* SIGCHLD == SIG_IGN auto-reaps children on Linux.  Inheriting that from a
     * library caller would make waitpid report ECHILD forever.  Block it
     * outside ppoll, then atomically unblock it while waiting: that removes the
     * check-then-sleep race without waking every idle broker ten times a
     * second just to call waitpid. */
    action.sa_handler = handle_server_child;
    action.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) != 0) return -1;
    action.sa_handler = SIG_IGN;
    action.sa_flags = 0;
    for (index = 0; index < sizeof ignored / sizeof ignored[0]; index++) {
        if (sigaction(ignored[index], &action, NULL) != 0) return -1;
    }
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    return sigprocmask(SIG_SETMASK, &mask, NULL);
}

static bool
server_owns_fd(const server_state *server, int ready_fd, int fd) {
    return fd <= STDERR_FILENO || fd == ready_fd || fd == server->listener_fd ||
        fd == server->pty_fd || fd == server->journal_fd ||
        fd == server->transcript_fd;
}

/* kpb_spawn() is a library boundary, so its caller may have arbitrary open
 * pipes, files, and sockets.  The command must inherit those descriptors just
 * as it would without the broker wrapper, but the persistent broker must not:
 * retaining one can postpone EOF or keep an unrelated resource alive for the
 * whole terminal session.  Run this in the forkpty parent, after the command
 * child has inherited its launch environment. */
static void
close_inherited_fds(server_state *server, int ready_fd) {
    DIR *directory = opendir("/proc/self/fd");
    if (directory) {
        struct dirent *entry;
        int directory_fd = dirfd(directory);
        while ((entry = readdir(directory))) {
            char *end = NULL;
            long value;
            errno = 0;
            value = strtol(entry->d_name, &end, 10);
            if (errno || !end || *end || value < 0 || value > INT_MAX) continue;
            if ((int)value == directory_fd ||
                server_owns_fd(server, ready_fd, (int)value)) {
                continue;
            }
            (void)close((int)value);
        }
        closedir(directory);
        return;
    }
    {
        long maximum = sysconf(_SC_OPEN_MAX);
        int fd;
        if (maximum < 0) maximum = 1024;
        if (maximum > INT_MAX) maximum = INT_MAX;
        for (fd = STDERR_FILENO + 1; fd < (int)maximum; fd++) {
            if (!server_owns_fd(server, ready_fd, fd)) (void)close(fd);
        }
    }
}

static int
server_main(const kpb_spawn_options *options, const char *session_id, int ready_fd) {
    server_state server;
    server_ready ready;
    int null_fd;
    int exit_code = 255;
    memset(&server, 0, sizeof server);
    server.listener_fd = server.pty_fd = server.journal_fd = server.client_fd = -1;
    server.transcript_fd = -1;
    {
        /* Before the first goto fail: memset leaves these at 0, and
         * cleanup_server would then close descriptor 0. */
        size_t slot;
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            server.observers[slot].fd = -1;
        }
    }
    server.child_pid = -1;
    server.started_millis = realtime_millis();
    server.journal_limit = options->journal_limit;
    server.transcript_limit = options->transcript_limit;
    server.transcript_graphics = options->transcript_graphics;
    server.transcript_scan = TRANSCRIPT_TEXT;
    server.journal_complete = true;
    server.size.ws_row = options->rows ? options->rows : 24;
    server.size.ws_col = options->columns ? options->columns : 80;
    server.size.ws_xpixel = options->xpixel;
    server.size.ws_ypixel = options->ypixel;
    copy_string(server.session_id, sizeof server.session_id, session_id);
    copy_string(server.cwd, sizeof server.cwd, options->cwd);
    build_command(server.command, options->argv);
    if (build_paths(options->runtime_dir, session_id, &server.paths) != KPB_OK) goto fail;
    if (setsid() < 0) goto fail;
    if (configure_server_signals() != 0) goto fail;
    null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd >= 0) {
        (void)dup2(null_fd, STDIN_FILENO);
        (void)dup2(null_fd, STDOUT_FILENO);
        (void)dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) close(null_fd);
    }
    server.listener_fd = create_listener(server.paths.socket_path);
    if (server.listener_fd < 0) goto fail;
    server.journal_fd = open(
        server.paths.journal_path,
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600
    );
    if (server.journal_fd < 0) goto fail;
    if (options->transcript_path && options->transcript_path[0] == '/') {
        /* Existing transcripts are continued rather than replaced, so a
         * recovered pane keeps its history.  Deliberately NOT O_APPEND: on
         * Linux pwrite() ignores the offset on an O_APPEND descriptor and
         * appends instead, which would corrupt rotation.  O_NOFOLLOW stops a
         * planted symlink from redirecting session output.  A transcript that
         * cannot be opened is reported by its absence, never by refusing to
         * start the pane. */
        struct stat transcript_status;
        server.transcript_fd = open(
            options->transcript_path,
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            0600
        );
        if (server.transcript_fd >= 0) {
            if (fstat(server.transcript_fd, &transcript_status) == 0 &&
                S_ISREG(transcript_status.st_mode) &&
                transcript_status.st_uid == geteuid() &&
                fchmod(server.transcript_fd, 0600) == 0 &&
                lseek(server.transcript_fd, 0, SEEK_END) >= 0) {
                server.transcript_bytes = (uint64_t)transcript_status.st_size;
                if (server.transcript_limit &&
                    server.transcript_bytes > server.transcript_limit) {
                    uint64_t keep = server.transcript_limit -
                        server.transcript_limit / 4U;
                    if (rotate_transcript(&server, keep) != 0) {
                        close(server.transcript_fd);
                        server.transcript_fd = -1;
                    }
                }
            } else {
                close(server.transcript_fd);
                server.transcript_fd = -1;
            }
        }
        record_transcript_size(&server);
    }
    server.child_pid = forkpty(&server.pty_fd, NULL, NULL, &server.size);
    if (server.child_pid < 0) goto fail;
    if (server.child_pid == 0) {
        if (reset_child_signals() != 0) _exit(126);
        close(ready_fd);
        close(server.listener_fd);
        close(server.journal_fd);
        if (server.transcript_fd >= 0) close(server.transcript_fd);
        if (chdir(options->cwd) != 0) _exit(126);
        setenv("KITTY_PTY_BROKER", "1", 1);
        setenv("KITTY_PTY_BROKER_SESSION", session_id, 1);
        execvp(options->argv[0], options->argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    close_inherited_fds(&server, ready_fd);
    {
        int flags = fcntl(server.pty_fd, F_GETFL);
        if (flags < 0 || fcntl(server.pty_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            goto fail;
        }
    }
    if (write_metadata(&server) != 0) goto fail;
    memset(&ready, 0, sizeof ready);
    ready.broker_pid = getpid();
    ready.child_pid = server.child_pid;
    if (write_all_fd(ready_fd, &ready, sizeof ready) < 0) goto fail_after_ready;
    close(ready_fd);
    exit_code = server_loop(&server);
    if (exit_code < 0 && server.child_pid > 0) {
        signal_child_session(server.child_pid, SIGKILL);
        while (waitpid(server.child_pid, NULL, 0) < 0 && errno == EINTR) {
            /* retry */
        }
    }
    cleanup_server(&server);
    return exit_code < 0 ? 255 : exit_code;

fail:
    memset(&ready, 0, sizeof ready);
    ready.error_number = errno ? errno : EIO;
    (void)write_all_fd(ready_fd, &ready, sizeof ready);
fail_after_ready:
    if (ready_fd >= 0) close(ready_fd);
    if (server.child_pid > 0) {
        (void)killpg(server.child_pid, SIGKILL);
        (void)kill(server.child_pid, SIGKILL);
        (void)waitpid(server.child_pid, NULL, 0);
    }
    cleanup_server(&server);
    return 255;
}

typedef struct {
    long pid;
    bool have_started;
    uint64_t started_millis;
    bool have_boot_id;
    char boot_id[64];
    bool have_ticks;
    uint64_t start_ticks;
} metadata_info;

/* Parse the session metadata.  Returns false when broker_pid is missing or
 * malformed, which leaves the directory alone.  The identity fields added
 * later are optional, and a malformed one is treated as absent rather than as
 * evidence: it can only ever weaken a proof, never supply one. */
static bool
parse_metadata(const char *data, metadata_info *info) {
    const char *line;
    memset(info, 0, sizeof *info);
    info->pid = -1;
    for (line = data; line && *line;) {
        char *end = NULL;
        errno = 0;
        if (strncmp(line, "broker_pid=", 11) == 0) {
            info->pid = strtol(line + 11, &end, 10);
            if (errno || !end || (*end != '\n' && *end != '\0') || info->pid <= 0) {
                return false;
            }
        } else if (strncmp(line, "started_millis=", 15) == 0) {
            unsigned long long value = strtoull(line + 15, &end, 10);
            if (!errno && end && end != line + 15 && (*end == '\n' || *end == '\0') &&
                line[15] >= '0' && line[15] <= '9') {
                info->started_millis = (uint64_t)value;
                info->have_started = true;
            }
        } else if (strncmp(line, "boot_id=", 8) == 0) {
            size_t size = strcspn(line + 8, "\n");
            if (size > 0 && size < sizeof info->boot_id) {
                memcpy(info->boot_id, line + 8, size);
                info->boot_id[size] = '\0';
                info->have_boot_id = true;
            }
        } else if (strncmp(line, "start_ticks=", 12) == 0) {
            unsigned long long value = strtoull(line + 12, &end, 10);
            if (!errno && end && end != line + 12 && (*end == '\n' || *end == '\0') &&
                line[12] >= '0' && line[12] <= '9') {
                info->start_ticks = (uint64_t)value;
                info->have_ticks = true;
            }
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return info->pid > 0;
}

/* The metadata file's consumer: decide whether a session directory is a
 * corpse.  The broker cannot clean up after SIGKILL or the OOM killer, and a
 * directory it leaves behind would otherwise be permanent - invisible to
 * list, blocking its session ID against respawn, and charging every future
 * list a connect() to a dead socket.
 *
 * Every path here is deliberately conservative: anything short of positive
 * proof that the recorded broker process is gone leaves the directory alone,
 * so a live or merely slow session is never destroyed.  Proof is one of:
 *
 *   - the recorded pid does not exist (ESRCH);
 *   - the recorded boot_id is not this boot's, so the pid belongs to a machine
 *     that no longer exists - the case a bare pid check gets wrong, because a
 *     persistent runtime outlives the reboot and the pid is then recycled;
 *   - the pid exists but its start time differs from the recorded start_ticks,
 *     so it is a different process that reused the number.
 *
 * Metadata without the identity fields (written by an earlier build) is held
 * to the pid rule alone, and an identity that cannot be read now - no /proc -
 * proves nothing. */
static bool
session_is_stale(const session_paths *paths) {
    char data[2048];
    metadata_info info;
    char current[64];
    uint64_t ticks;
    ssize_t size = read_small_file(paths->metadata_path, data, sizeof data);
    if (size <= 0) return false;
    if (!parse_metadata(data, &info)) return false;
    if (kill((pid_t)info.pid, 0) != 0 && errno == ESRCH) return true;
    if (info.have_boot_id && current_boot_id(current) == 0 &&
        strcmp(info.boot_id, current) != 0) {
        return true;
    }
    if (info.have_ticks && process_start_ticks(info.pid, &ticks) == 0 &&
        ticks != info.start_ticks) {
        return true;
    }
    return false;
}

static uint64_t
reaped_limit_from_environment(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;
    if (!value || value[0] < '0' || value[0] > '9') return fallback;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno || !end || *end != '\0' || parsed == 0 || parsed > INT64_MAX) return fallback;
    return (uint64_t)parsed;
}

/* "ID.STARTED_MILLIS.journal".  Session IDs may contain '.', so the millis are
 * the part after the LAST dot. */
static bool
parse_archive_name(const char *name, char id[KPB_SESSION_ID_MAX + 1], uint64_t *started) {
    static const char suffix[] = ".journal";
    size_t length = strlen(name);
    size_t cut;
    size_t digits;
    uint64_t value = 0;
    size_t index;
    if (length <= sizeof suffix - 1 ||
        strcmp(name + length - (sizeof suffix - 1), suffix) != 0) {
        return false;
    }
    length -= sizeof suffix - 1;
    cut = length;
    while (cut > 0 && name[cut - 1] != '.') cut--;
    if (cut <= 1) return false;
    digits = length - cut;
    if (digits == 0 || digits > 19) return false;
    for (index = cut; index < length; index++) {
        if (name[index] < '0' || name[index] > '9') return false;
        value = value * 10U + (uint64_t)(name[index] - '0');
    }
    if (cut - 1 > KPB_SESSION_ID_MAX) return false;
    memcpy(id, name, cut - 1);
    id[cut - 1] = '\0';
    if (!valid_component(id)) return false;
    *started = value;
    return true;
}

typedef struct {
    char name[KPB_SESSION_ID_MAX + 64];
    uint64_t started;
    uint64_t journal_bytes;
    uint64_t meta_bytes;
    struct timespec age;
} archive_item;

static int
compare_archive_items(const void *left_opaque, const void *right_opaque) {
    const archive_item *left = left_opaque;
    const archive_item *right = right_opaque;
    if (left->age.tv_sec != right->age.tv_sec) {
        return left->age.tv_sec < right->age.tv_sec ? -1 : 1;
    }
    if (left->age.tv_nsec != right->age.tv_nsec) {
        return left->age.tv_nsec < right->age.tv_nsec ? -1 : 1;
    }
    if (left->started != right->started) return left->started < right->started ? -1 : 1;
    return strcmp(left->name, right->name);
}

/* The .meta file that rides along with "NAME.journal", in place. */
static int
meta_path_for(char *path, size_t capacity) {
    size_t length = strlen(path);
    if (length <= sizeof ".journal" - 1 ||
        length - (sizeof ".journal" - 1) + sizeof ".meta" > capacity) {
        return -1;
    }
    memcpy(path + length - (sizeof ".journal" - 1), ".meta", sizeof ".meta");
    return 0;
}

/* Remove a journal and its .meta from the archive. */
static void
remove_archive_item(const char *reaped_dir, const archive_item *item) {
    char path[KPB_PATH_MAX];
    if (join_path(path, sizeof path, reaped_dir, item->name) != 0) return;
    (void)unlink(path);
    if (meta_path_for(path, sizeof path) == 0) (void)unlink(path);
}

/* Keep RUNTIME/reaped/ inside its bounds, evicting the oldest first.  Age is
 * the .meta file's mtime - written when the session was reaped - falling back
 * to the journal's.  The bound is on journals (their .meta files ride along
 * and count toward the bytes, not the file count), because a standalone
 * install has nothing else that would ever empty this directory. */
static void
evict_reaped(const char *reaped_dir) {
    uint64_t max_bytes = reaped_limit_from_environment(
        "KITTY_PTY_BROKER_REAPED_MAX_BYTES", KPB_DEFAULT_REAPED_MAX_BYTES);
    uint64_t max_files = reaped_limit_from_environment(
        "KITTY_PTY_BROKER_REAPED_MAX_FILES", KPB_DEFAULT_REAPED_MAX_FILES);
    archive_item *items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t index;
    uint64_t total = 0;
    struct dirent *entry;
    DIR *directory = opendir(reaped_dir);
    if (!directory) return;
    while ((entry = readdir(directory))) {
        archive_item item;
        char id[KPB_SESSION_ID_MAX + 1];
        char path[KPB_PATH_MAX];
        struct stat journal;
        struct stat meta;
        memset(&item, 0, sizeof item);
        if (!parse_archive_name(entry->d_name, id, &item.started)) continue;
        if (join_path(path, sizeof path, reaped_dir, entry->d_name) != 0 ||
            lstat(path, &journal) != 0 || !S_ISREG(journal.st_mode)) {
            continue;
        }
        copy_string(item.name, sizeof item.name, entry->d_name);
        item.journal_bytes = (uint64_t)journal.st_size;
        item.age = journal.st_mtim;
        if (meta_path_for(path, sizeof path) == 0 &&
            lstat(path, &meta) == 0 && S_ISREG(meta.st_mode)) {
            item.meta_bytes = (uint64_t)meta.st_size;
            item.age = meta.st_mtim;
        }
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2U : 32U;
            archive_item *replacement = realloc(items, grown * sizeof *items);
            if (!replacement) break;
            items = replacement;
            capacity = grown;
        }
        items[count++] = item;
    }
    closedir(directory);
    if (count > 1) qsort(items, count, sizeof *items, compare_archive_items);
    for (index = 0; index < count; index++) total += items[index].journal_bytes + items[index].meta_bytes;
    for (index = 0; index < count && (count - index > max_files || total > max_bytes); index++) {
        remove_archive_item(reaped_dir, &items[index]);
        total -= items[index].journal_bytes + items[index].meta_bytes;
    }
    free(items);
}

/* Keep a dead session's journal instead of deleting it.  The journal is what
 * the screen looked like when the session died, which is exactly what someone
 * investigating an OOM kill wants and exactly what deleting the directory
 * destroyed.  A rename within one filesystem is atomic and O(1) however large
 * the journal is, so this adds nothing a list walk would notice.
 *
 * Best-effort in every step, and a failure falls back to the old behaviour
 * (the journal is deleted with the directory): an archive that cannot be made
 * - reaped/ unwritable, a foreign or symlinked reaped/, another filesystem -
 * must never leave a corpse that blocks `run --id` from respawning. */
static void
archive_journal(const session_paths *paths) {
    char data[2048];
    char destination[KPB_PATH_MAX];
    char meta_path[KPB_PATH_MAX];
    metadata_info info;
    struct stat journal;
    struct stat probe;
    ssize_t size;
    uint64_t started = 0;
    int fd;
    if (lstat(paths->journal_path, &journal) != 0 || !S_ISREG(journal.st_mode) ||
        journal.st_uid != geteuid() || journal.st_size <= 0) {
        return;
    }
    size = read_small_file(paths->metadata_path, data, sizeof data);
    if (size < 0) size = 0;
    if (size > 0 && parse_metadata(data, &info) && info.have_started) {
        started = info.started_millis;
    }
    if (ensure_private_directory(paths->reaped_dir, true) != KPB_OK) return;
    if (snprintf(
            destination, sizeof destination, "%s/%s.%llu.journal",
            paths->reaped_dir, paths->session_id, (unsigned long long)started
        ) >= (int)sizeof destination ||
        snprintf(
            meta_path, sizeof meta_path, "%s/%s.%llu.meta",
            paths->reaped_dir, paths->session_id, (unsigned long long)started
        ) >= (int)sizeof meta_path) {
        return;
    }
    if (lstat(destination, &probe) == 0 || lstat(meta_path, &probe) == 0) return;
    if (rename(paths->journal_path, destination) != 0) return;
    (void)chmod(destination, 0600);
    fd = open(meta_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        char tail[64];
        int tail_size = snprintf(
            tail, sizeof tail, "%sreaped_millis=%llu\n",
            size > 0 && data[size - 1] != '\n' ? "\n" : "",
            (unsigned long long)realtime_millis());
        (void)fchmod(fd, 0600);
        if ((size > 0 && write_all_fd(fd, data, (size_t)size) < 0) ||
            tail_size < 0 || write_all_fd(fd, tail, (size_t)tail_size) < 0) {
            /* A journal without its .meta is still listed; one with a
             * truncated .meta is not worth keeping a half-written file for. */
            close(fd);
            (void)unlink(meta_path);
        } else {
            close(fd);
        }
    }
    evict_reaped(paths->reaped_dir);
}

/* Remove a directory session_is_stale() has vouched for, keeping its journal
 * first.  Best-effort, and rmdir is the commit point: it fails if anything
 * unexpected is still inside, which is the safe direction.  Callers hold the
 * sessions-directory lock across both the proof and this removal. */
static void
reap_stale_session(const session_paths *paths) {
    archive_journal(paths);
    (void)unlink(paths->socket_path);
    (void)unlink(paths->journal_path);
    (void)unlink(paths->metadata_path);
    (void)rmdir(paths->session_dir);
}

/* Serialise reaping against session creation.  The staleness proof and the
 * unlinks are two separate steps, and two same-uid callers respawning one ID
 * could otherwise interleave them: the first reaps the corpse and recreates
 * the directory, and the second - still holding its ESRCH proof from the old
 * metadata - unlinks the fresh socket and journal by path.  An exclusive
 * lock on the sessions directory makes proof, removal, and recreation one
 * step.
 *
 * The lock is taken NON-blocking and retried until `deadline`, never with a
 * blocking flock.  Holders are expected to release it within microseconds, but
 * "expected" is not a bound: a holder that is stopped, or stuck on a slow
 * filesystem, would otherwise hang every caller that needs the lock, and with
 * it `list` and the TUI refresh.  One attempt is always made, even when the
 * deadline has already passed, so an uncontended lock costs nothing.  Returns
 * the locked descriptor, or -1 (errno EWOULDBLOCK when it timed out). */
static int
lock_sessions_dir_until(const session_paths *paths, const struct timespec *deadline) {
    int fd = open(paths->sessions_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (;;) {
        long remaining;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
        if (errno != EWOULDBLOCK && errno != EINTR) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        remaining = millis_until(deadline);
        if (remaining <= 0) {
            close(fd);
            errno = EWOULDBLOCK;
            return -1;
        }
        (void)poll(NULL, 0, remaining < 5 ? (int)remaining : 5);
    }
}

kpb_result
kpb_spawn(const kpb_spawn_options *options, kpb_status *status) {
    session_paths paths;
    char generated[KPB_SESSION_ID_MAX + 1];
    const char *session_id;
    int ready_pipe[2];
    pid_t pid;
    server_ready ready;
    kpb_result result;
    struct stat existing;
    if (!options || !options->runtime_dir || !options->cwd ||
        !options->argv || !options->argv[0]) {
        return KPB_ERR_INVALID;
    }
    if (strlen(options->cwd) >= KPB_PATH_MAX ||
        options->journal_limit > INT64_MAX ||
        options->transcript_limit > INT64_MAX ||
        (options->transcript_path &&
         options->transcript_graphics != KPB_TRANSCRIPT_GRAPHICS_ELIDE &&
         options->transcript_graphics != KPB_TRANSCRIPT_GRAPHICS_KEEP)) {
        return KPB_ERR_INVALID;
    }
    session_id = options->session_id;
    if (!session_id || !*session_id) {
        result = kpb_generate_session_id(generated);
        if (result != KPB_OK) return result;
        session_id = generated;
    }
    if (kpb_validate_session_id(session_id) != KPB_OK) return KPB_ERR_INVALID;
    /* Every path is validated BEFORE anything is created.  A socket path over
     * the 107-byte limit cannot be bound, and finding that out after
     * kpb_prepare_runtime would leave runtime/ and sessions/ behind for a
     * spawn that was refused. */
    result = build_paths(options->runtime_dir, session_id, &paths);
    if (result != KPB_OK) return result;
    result = kpb_prepare_runtime(options->runtime_dir);
    if (result != KPB_OK) return result;
    result = build_paths(options->runtime_dir, session_id, &paths);
    if (result != KPB_OK) return result;
    {
        /* Everything from the staleness proof to mkdir happens under the
         * sessions-directory lock: a concurrent respawn or list walk could
         * otherwise reap the directory this call has just recreated. */
        struct timespec lock_deadline;
        int lock_fd;
        deadline_in(&lock_deadline, KPB_DEFAULT_TIMEOUT_MILLIS);
        lock_fd = lock_sessions_dir_until(&paths, &lock_deadline);
        /* Bounded like every other wait: a holder that is stopped is reported,
         * and nothing has been created yet. */
        if (lock_fd < 0) return errno == EWOULDBLOCK ? KPB_ERR_TIMEOUT : KPB_ERR_SYSTEM;
        if (lstat(paths.session_dir, &existing) == 0) {
            /* A leftover directory whose broker is provably gone must not
             * block this ID forever: a stable `run --id` pane could otherwise
             * never be respawned after the machine OOM-killed its broker. */
            if (!session_is_stale(&paths)) {
                close(lock_fd);
                return KPB_ERR_EXISTS;
            }
            reap_stale_session(&paths);
            if (lstat(paths.session_dir, &existing) == 0) {
                close(lock_fd);
                return KPB_ERR_EXISTS;
            }
            if (errno != ENOENT) {
                close(lock_fd);
                return KPB_ERR_SYSTEM;
            }
        } else if (errno != ENOENT) {
            close(lock_fd);
            return KPB_ERR_SYSTEM;
        }
        if (mkdir(paths.session_dir, 0700) != 0) {
            bool taken = errno == EEXIST;
            close(lock_fd);
            return taken ? KPB_ERR_EXISTS : KPB_ERR_SYSTEM;
        }
        close(lock_fd);
    }
    if (pipe2(ready_pipe, O_CLOEXEC) != 0) {
        rmdir(paths.session_dir);
        return KPB_ERR_SYSTEM;
    }
    pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        rmdir(paths.session_dir);
        errno = saved;
        return KPB_ERR_SYSTEM;
    }
    if (pid == 0) {
        int code;
        close(ready_pipe[0]);
        code = server_main(options, session_id, ready_pipe[1]);
        _exit(code);
    }
    close(ready_pipe[1]);
    if (read_all_fd(ready_pipe[0], &ready, sizeof ready) < 0) {
        int saved = errno;
        close(ready_pipe[0]);
        errno = saved;
        return KPB_ERR_CHILD;
    }
    close(ready_pipe[0]);
    if (ready.error_number) {
        errno = ready.error_number;
        return KPB_ERR_CHILD;
    }
    if (status) {
        result = kpb_query_status(options->runtime_dir, session_id, status);
        if (result != KPB_OK) {
            memset(status, 0, sizeof *status);
            copy_string(status->session_id, sizeof status->session_id, session_id);
            status->broker_pid = (pid_t)ready.broker_pid;
            status->child_pid = (pid_t)ready.child_pid;
        }
    }
    return KPB_OK;
}

kpb_result
kpb_attach_timeout(
    const char *runtime_dir,
    const char *session_id,
    unsigned short rows,
    unsigned short columns,
    unsigned short xpixel,
    unsigned short ypixel,
    kpb_connection *connection,
    int timeout_millis
) {
    kpb_wire_winsize size;
    struct timespec deadline;
    int fd;
    kpb_result result;
    if (!connection) return KPB_ERR_INVALID;
    memset(connection, 0, sizeof *connection);
    connection->fd = -1;
    deadline_from_timeout(&deadline, timeout_millis, KPB_DEFAULT_TIMEOUT_MILLIS);
    result = connect_session_until(runtime_dir, session_id, &fd, &deadline);
    if (result != KPB_OK) return result;
    size.rows = htons(rows);
    size.columns = htons(columns);
    size.xpixel = htons(xpixel);
    size.ypixel = htons(ypixel);
    result = send_frame(fd, KPB_FRAME_ATTACH, &size, sizeof size);
    if (result != KPB_OK) {
        close(fd);
        return result;
    }
    /* A version-1 attach has no reply frame, so "the broker took it" can only
     * be seen as the broker's first frame: the replay (at least REPLAY_DONE) or
     * a refusal, both of which a live broker sends at once.  The wait is for
     * that frame to be COMPLETE, not for its first byte: a peer that sends one
     * byte and stops would otherwise pass this check and hang the caller in
     * its first read.  The frame is only peeked - the caller reads it - so
     * nothing is consumed.  A first frame too large to peek (over one I/O
     * chunk) is waited for through its header only; the rest is read under
     * kpb_receive's own bound. */
    for (;;) {
        unsigned char peeked[sizeof(kpb_frame_header) + KPB_IO_CHUNK];
        long remaining = millis_until(&deadline);
        ssize_t count;
        if (remaining <= 0) {
            close(fd);
            return KPB_ERR_TIMEOUT;
        }
        count = recv(fd, peeked, sizeof peeked, MSG_PEEK | MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            int saved = errno;
            close(fd);
            errno = saved;
            return KPB_ERR_SYSTEM;
        }
        if (count == 0) break;  /* closed: the caller's read reports it */
        if (count > 0 && (size_t)count >= sizeof(kpb_frame_header)) {
            kpb_frame_header header;
            size_t payload;
            size_t need;
            memcpy(&header, peeked, sizeof header);
            payload = ntohl(header.payload_size);
            if (ntohl(header.magic) != KPB_PROTOCOL_MAGIC ||
                ntohs(header.version) != KPB_PROTOCOL_VERSION ||
                payload > KPB_PROTOCOL_MAX_PAYLOAD) {
                break;  /* not a frame: the caller's read reports it */
            }
            need = sizeof header + (payload < KPB_IO_CHUNK ? payload : KPB_IO_CHUNK);
            if ((size_t)count >= need) break;
        }
        if (count < 0) {
            /* Nothing yet: wait for the first byte. */
            struct pollfd waiting = {.fd = fd, .events = POLLIN, .revents = 0};
            (void)poll(&waiting, 1, (int)(remaining < 1000 ? remaining : 1000));
        } else {
            /* Some of a frame is here; poll would return at once, so wait a
             * moment for the rest. */
            (void)poll(NULL, 0, remaining < 5 ? (int)remaining : 5);
        }
    }
    connection->fd = fd;
    copy_string(connection->session_id, sizeof connection->session_id, session_id);
    return KPB_OK;
}

kpb_result
kpb_attach(
    const char *runtime_dir,
    const char *session_id,
    unsigned short rows,
    unsigned short columns,
    unsigned short xpixel,
    unsigned short ypixel,
    kpb_connection *connection
) {
    return kpb_attach_timeout(
        runtime_dir, session_id, rows, columns, xpixel, ypixel, connection, 0);
}

void
kpb_attach_options_init(kpb_attach_options *options) {
    if (!options) return;
    memset(options, 0, sizeof *options);
    options->mode = KPB_ATTACH_CONTROL;
    options->max_version = KPB_PROTOCOL_VERSION_MAX;
}

int
kpb_protocol_version_max(void) {
    return KPB_PROTOCOL_VERSION_MAX;
}

/* There is deliberately no implicit downgrade to version 1 here.  The shipped
 * attach path is kpb_attach(), which speaks version 1 on the wire and is
 * therefore safe against a broker left running by a previous build; a caller
 * that asks for observe or resume is asking for something an older broker
 * cannot provide, and is told so rather than silently given less. */
kpb_result
kpb_attach_with_options(
    const char *runtime_dir,
    const char *session_id,
    const kpb_attach_options *options,
    kpb_connection *connection,
    kpb_attach_result *result
) {
    return kpb_attach_with_options_timeout(
        runtime_dir, session_id, options, connection, result, 0);
}

kpb_result
kpb_attach_with_options_timeout(
    const char *runtime_dir,
    const char *session_id,
    const kpb_attach_options *options,
    kpb_connection *connection,
    kpb_attach_result *result,
    int timeout_millis
) {
    struct timespec deadline;
    kpb_wire_attach request;
    kpb_wire_attach_reply reply;
    unsigned char staging[256];
    uint16_t type = 0;
    uint16_t code;
    uint32_t payload_size = 0;
    uint32_t flags;
    bool observing;
    uint16_t requested_version;
    uint16_t selected_version;
    int fd = -1;
    kpb_result outcome;

    if (!connection) return KPB_ERR_INVALID;
    memset(connection, 0, sizeof *connection);
    connection->fd = -1;
    if (result) memset(result, 0, sizeof *result);
    if (!options || options->max_version < 1) return KPB_ERR_INVALID;
    observing = options->mode == KPB_ATTACH_OBSERVE;
    if (options->mode != KPB_ATTACH_CONTROL && !observing) return KPB_ERR_INVALID;
    if ((observing || options->resume) && options->max_version < 2) {
        return KPB_ERR_INVALID;
    }
    if (options->max_version < 2) {
        outcome = kpb_attach_timeout(
            runtime_dir, session_id,
            options->rows, options->columns,
            options->xpixel, options->ypixel, connection, timeout_millis);
        if (outcome == KPB_OK && result) result->version = 1;
        return outcome;
    }

    deadline_from_timeout(&deadline, timeout_millis, KPB_DEFAULT_TIMEOUT_MILLIS);
    outcome = connect_session_until(runtime_dir, session_id, &fd, &deadline);
    if (outcome != KPB_OK) return outcome;

    memset(&request, 0, sizeof request);
    /* An observer sends zero dimensions: it must not be able to influence the
     * pane even if a future server were to read them. */
    request.rows = htons(observing ? 0 : options->rows);
    request.columns = htons(observing ? 0 : options->columns);
    request.xpixel = htons(observing ? 0 : options->xpixel);
    request.ypixel = htons(observing ? 0 : options->ypixel);
    requested_version = options->max_version > KPB_PROTOCOL_VERSION_MAX
        ? (uint16_t)KPB_PROTOCOL_VERSION_MAX : (uint16_t)options->max_version;
    request.version = htons(requested_version);
    request.mode = htons(
        observing ? (uint16_t)KPB_WIRE_MODE_OBSERVE : (uint16_t)KPB_WIRE_MODE_CONTROL);
    request.flags = htonl(options->resume ? KPB_ATTACH_FLAG_RESUME : 0U);
    request.resume_epoch = host_to_be64(options->resume_epoch);
    request.resume_offset = host_to_be64(options->resume_offset);

    outcome = send_frame(
        fd,
        observing ? (uint16_t)KPB_FRAME_OBSERVE : (uint16_t)KPB_FRAME_ATTACH,
        &request, sizeof request);
    if (outcome != KPB_OK) {
        close(fd);
        return outcome;
    }
    /* A broker that predates version 2 answers with ERROR "invalid request",
     * which lands here as a type mismatch and becomes a protocol error. */
    outcome = receive_frame_bounded(
        fd, &type, staging, sizeof staging, &payload_size, &deadline);
    if (outcome != KPB_OK) {
        close(fd);
        return outcome;
    }
    if (type != KPB_FRAME_ATTACH_REPLY || payload_size != sizeof reply) {
        close(fd);
        return KPB_ERR_PROTOCOL;
    }
    memcpy(&reply, staging, sizeof reply);
    code = ntohs(reply.result);
    if (code != 0) {
        close(fd);
        return code <= KPB_ERR_CHILD ? (kpb_result)code : KPB_ERR_PROTOCOL;
    }
    flags = ntohl(reply.flags);
    selected_version = ntohs(reply.version);
    if (selected_version < 2 || selected_version > KPB_PROTOCOL_VERSION_MAX ||
        selected_version > requested_version ||
        (flags & ~(KPB_REPLY_FLAG_RESUMED | KPB_REPLY_FLAG_COMPLETE |
                   KPB_REPLY_FLAG_TRUNCATED))) {
        close(fd);
        return KPB_ERR_PROTOCOL;
    }
    if (result) {
        result->version = selected_version;
        result->resumed = (flags & KPB_REPLY_FLAG_RESUMED) != 0;
        result->truncated = (flags & KPB_REPLY_FLAG_TRUNCATED) != 0;
        result->replay_complete = (flags & KPB_REPLY_FLAG_COMPLETE) != 0;
        result->journal_epoch = be64_to_host(reply.journal_epoch);
        result->journal_offset = be64_to_host(reply.journal_offset);
    }
    connection->fd = fd;
    copy_string(connection->session_id, sizeof connection->session_id, session_id);
    return KPB_OK;
}

kpb_result
kpb_observe(
    const char *runtime_dir,
    const char *session_id,
    kpb_connection *connection,
    kpb_attach_result *result
) {
    kpb_attach_options options;
    kpb_attach_options_init(&options);
    options.mode = KPB_ATTACH_OBSERVE;
    return kpb_attach_with_options(
        runtime_dir, session_id, &options, connection, result);
}

kpb_result
kpb_send_input(kpb_connection *connection, const void *data, size_t size) {
    if (!connection || connection->fd < 0 || (!data && size)) return KPB_ERR_INVALID;
    if (size > KPB_IO_CHUNK) return KPB_ERR_INVALID;
    return send_frame(connection->fd, KPB_FRAME_INPUT, data, (uint32_t)size);
}

kpb_result
kpb_resize(
    kpb_connection *connection,
    unsigned short rows,
    unsigned short columns,
    unsigned short xpixel,
    unsigned short ypixel
) {
    kpb_wire_winsize size;
    if (!connection || connection->fd < 0) return KPB_ERR_INVALID;
    size.rows = htons(rows);
    size.columns = htons(columns);
    size.xpixel = htons(xpixel);
    size.ypixel = htons(ypixel);
    return send_frame(connection->fd, KPB_FRAME_RESIZE, &size, sizeof size);
}

kpb_result
kpb_receive(
    kpb_connection *connection,
    void *buffer,
    size_t capacity,
    kpb_event *event
) {
    uint16_t type;
    uint32_t payload_size;
    kpb_result result;
    if (!connection || connection->fd < 0 || !event || !buffer ||
        capacity < sizeof(kpb_wire_exit)) {
        return KPB_ERR_INVALID;
    }
    memset(event, 0, sizeof *event);
    /* Waiting for a frame to BEGIN is unbounded: this is the live stream, and
     * a quiet pane is not a fault.  Once any byte of a frame has arrived the
     * rest of it is read under a deadline, so a peer that sends half a frame and
     * stops cannot hang the caller.  A non-blocking descriptor keeps its
     * contract of failing at once when nothing is there. */
    {
        int flags = fcntl(connection->fd, F_GETFL, 0);
        if (flags >= 0 && (flags & O_NONBLOCK)) {
            char probe;
            if (recv(connection->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT) < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return KPB_ERR_SYSTEM;
            }
        } else {
            struct pollfd waiting = {.fd = connection->fd, .events = POLLIN, .revents = 0};
            int ready;
            do {
                ready = poll(&waiting, 1, -1);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0) return KPB_ERR_SYSTEM;
        }
    }
    {
        struct timespec deadline;
        deadline_in(&deadline, KPB_FRAME_TIMEOUT_MILLIS);
        result = receive_frame_bounded(
            connection->fd, &type, buffer, capacity, &payload_size, &deadline);
    }
    if (result != KPB_OK) {
        /* A frame that does not fit has already been consumed off the stream
         * to preserve framing, so its payload is unrecoverable here.  Report
         * what was lost - the size and the event it would have been - instead
         * of returning a bare error that hides the loss entirely.  Skipped
         * OUTPUT is journal content, so a protocol-2 caller that keeps a
         * cursor can recover the bytes by reattaching with resume from it. */
        if (result == KPB_ERR_BUFFER) {
            switch (type) {
                case KPB_FRAME_OUTPUT: event->type = KPB_EVENT_OUTPUT; break;
                case KPB_FRAME_RESET: event->type = KPB_EVENT_RESET; break;
                case KPB_FRAME_ERROR: event->type = KPB_EVENT_ERROR; break;
                default: break;
            }
            event->size = payload_size;
        }
        return result;
    }
    switch (type) {
        case KPB_FRAME_OUTPUT:
            event->type = KPB_EVENT_OUTPUT;
            event->size = payload_size;
            return KPB_OK;
        case KPB_FRAME_RESET:
            event->type = KPB_EVENT_RESET;
            event->size = payload_size;
            return KPB_OK;
        case KPB_FRAME_REPLAY_DONE:
            if (payload_size != 0) return KPB_ERR_PROTOCOL;
            event->type = KPB_EVENT_REPLAY_DONE;
            return KPB_OK;
        case KPB_FRAME_EXIT: {
            /* The caller chooses this buffer and may legitimately hand us an
             * offset into a byte array, so it carries no alignment guarantee.
             * Casting it to the wire struct is undefined behaviour. */
            kpb_wire_exit wire;
            if (payload_size != sizeof wire) return KPB_ERR_PROTOCOL;
            memcpy(&wire, buffer, sizeof wire);
            event->type = KPB_EVENT_EXIT;
            {
                uint32_t wait_status = ntohl(wire.wait_status);
                if (wait_status > INT_MAX) return KPB_ERR_PROTOCOL;
                event->exit_status = (int)wait_status;
            }
            return KPB_OK;
        }
        case KPB_FRAME_ERROR:
            event->type = KPB_EVENT_ERROR;
            event->size = payload_size;
            return KPB_OK;
        default:
            return KPB_ERR_PROTOCOL;
    }
}

void
kpb_detach(kpb_connection *connection) {
    if (!connection) return;
    if (connection->fd >= 0) {
        (void)send_frame(connection->fd, KPB_FRAME_DETACH, NULL, 0);
        close(connection->fd);
    }
    connection->fd = -1;
}

kpb_result
kpb_query_status_timeout(
    const char *runtime_dir,
    const char *session_id,
    kpb_status *status,
    int timeout_millis
) {
    kpb_wire_status wire;
    struct timespec deadline;
    uint16_t type;
    uint32_t payload_size;
    int fd;
    kpb_result result;
    if (!status) return KPB_ERR_INVALID;
    deadline_from_timeout(&deadline, timeout_millis, KPB_DEFAULT_TIMEOUT_MILLIS);
    result = connect_session_until(runtime_dir, session_id, &fd, &deadline);
    if (result != KPB_OK) return result;
    result = send_frame(fd, KPB_FRAME_STATUS, NULL, 0);
    if (result == KPB_OK) {
        result = receive_frame_bounded(
            fd, &type, &wire, sizeof wire, &payload_size, &deadline);
        if (result == KPB_OK &&
            (type != KPB_FRAME_STATUS_REPLY || payload_size != sizeof wire)) {
            result = KPB_ERR_PROTOCOL;
        }
    }
    close(fd);
    return result == KPB_OK ? wire_to_status(&wire, status) : result;
}

kpb_result
kpb_query_status(
    const char *runtime_dir,
    const char *session_id,
    kpb_status *status
) {
    return kpb_query_status_timeout(runtime_dir, session_id, status, 0);
}

kpb_result
kpb_terminate_timeout(
    const char *runtime_dir,
    const char *session_id,
    int timeout_millis
) {
    unsigned char payload[128];
    struct timespec deadline;
    uint16_t type;
    uint32_t payload_size;
    int fd;
    kpb_result result;
    deadline_from_timeout(&deadline, timeout_millis, KPB_DEFAULT_TIMEOUT_MILLIS);
    result = connect_session_until(runtime_dir, session_id, &fd, &deadline);
    if (result != KPB_OK) return result;
    result = send_frame(fd, KPB_FRAME_TERMINATE, NULL, 0);
    if (result == KPB_OK) {
        result = receive_frame_bounded(
            fd, &type, payload, sizeof payload, &payload_size, &deadline);
        if (result == KPB_OK && (type != KPB_FRAME_ACK || payload_size != 0)) {
            result = KPB_ERR_PROTOCOL;
        }
    }
    close(fd);
    return result;
}

kpb_result
kpb_terminate(const char *runtime_dir, const char *session_id) {
    return kpb_terminate_timeout(runtime_dir, session_id, 0);
}

/* --- listing ----------------------------------------------------------- */

static kpb_result
check_directory(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        if (errno == ENOENT) return KPB_ERR_NOT_FOUND;
        if (errno == ENOTDIR || errno == ELOOP) return KPB_ERR_SECURITY;
        return KPB_ERR_SYSTEM;
    }
    if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode) || status.st_uid != geteuid()) {
        return KPB_ERR_SECURITY;
    }
    return KPB_OK;
}

kpb_result
kpb_check_runtime(const char *runtime_dir) {
    if (!runtime_dir || runtime_dir[0] != '/') return KPB_ERR_INVALID;
    return check_directory(runtime_dir);
}

kpb_result
kpb_session_socket_path(
    const char *runtime_dir,
    const char *session_id,
    char *output,
    size_t capacity
) {
    char resolved[KPB_PATH_MAX];
    char path[KPB_PATH_MAX + KPB_SESSION_ID_MAX + 32];
    size_t length;
    kpb_result result;
    if (!output || capacity == 0) return KPB_ERR_INVALID;
    output[0] = '\0';
    if (!runtime_dir || runtime_dir[0] != '/' || kpb_validate_session_id(session_id) != KPB_OK) {
        return KPB_ERR_INVALID;
    }
    result = resolve_runtime(runtime_dir, resolved);
    if (result != KPB_OK) return result;
    length = (size_t)snprintf(
        path, sizeof path, "%s/sessions/%s/control.sock",
        strcmp(resolved, "/") == 0 ? "" : resolved, session_id);
    if (length < capacity) memcpy(output, path, length + 1);
    return length > KPB_SOCKET_PATH_LIMIT ? KPB_ERR_NAME_TOO_LONG : KPB_OK;
}

kpb_result
kpb_read_boot_id(char output[64]) {
    if (!output) return KPB_ERR_INVALID;
    return current_boot_id(output) == 0 ? KPB_OK : KPB_ERR_NOT_FOUND;
}

kpb_result
kpb_read_start_ticks(pid_t pid, uint64_t *ticks) {
    if (!ticks) return KPB_ERR_INVALID;
    return process_start_ticks((long)pid, ticks) == 0 ? KPB_OK : KPB_ERR_NOT_FOUND;
}

kpb_result
kpb_read_cwd_now(pid_t child_pid, char *output, size_t capacity) {
    char path[64];
    ssize_t size;
    if (!output || capacity < 2) return KPB_ERR_INVALID;
    output[0] = '\0';
    if (child_pid <= 0 ||
        snprintf(path, sizeof path, "/proc/%ld/cwd", (long)child_pid) >= (int)sizeof path) {
        return KPB_ERR_NOT_FOUND;
    }
    size = readlink(path, output, capacity - 1);
    /* readlink does not NUL-terminate, and a result that fills the buffer may
     * have been cut short, which would be a plausible but wrong directory. */
    if (size <= 0 || (size_t)size >= capacity - 1) {
        output[0] = '\0';
        return KPB_ERR_NOT_FOUND;
    }
    output[size] = '\0';
    return KPB_OK;
}

/* At most this many sessions are being queried at once, so a runtime with a
 * very large number of sessions cannot exhaust descriptors.  Sessions not yet
 * started when the deadline passes are reported as timed out. */
#define KPB_LIST_WINDOW 128

typedef enum { LIST_PENDING = 0, LIST_ACTIVE, LIST_DONE } list_state;

typedef struct {
    char id[KPB_SESSION_ID_MAX + 1];
    list_state state;
    kpb_result error;
    int fd;
    size_t received;
    unsigned char *buffer;
    kpb_status *status;
} list_slot;

#define LIST_REPLY_SIZE (sizeof(kpb_frame_header) + sizeof(kpb_wire_status))

static void
list_finish(list_slot *slot, kpb_result error) {
    if (slot->fd >= 0) close(slot->fd);
    slot->fd = -1;
    free(slot->buffer);
    slot->buffer = NULL;
    slot->error = error;
    slot->state = LIST_DONE;
}

/* Begin one status query without waiting: connect (non-blocking), check the
 * peer, send the request.  Returns -1 only when descriptors ran out and the
 * caller should retry once an in-flight query finishes. */
static int
list_start(const char *runtime_dir, list_slot *slot, bool can_defer) {
    session_paths paths;
    struct sockaddr_un address;
    kpb_result result = build_paths(runtime_dir, slot->id, &paths);
    int fd;
    slot->fd = -1;
    if (result != KPB_OK) {
        list_finish(slot, result);
        return 0;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        if ((errno == EMFILE || errno == ENFILE) && can_defer) return -1;
        list_finish(slot, KPB_ERR_SYSTEM);
        return 0;
    }
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    copy_string(address.sun_path, sizeof address.sun_path, paths.socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        int saved = errno;
        close(fd);
        /* EAGAIN: a full backlog.  The broker is not accepting, which is what
         * a stopped one looks like once enough dead connections have piled up
         * behind it. */
        list_finish(
            slot,
            saved == ENOENT || saved == ECONNREFUSED ? KPB_ERR_NOT_FOUND
            : saved == EAGAIN ? KPB_ERR_TIMEOUT : KPB_ERR_SYSTEM);
        return 0;
    }
    slot->fd = fd;
    if (!peer_is_owner(fd)) {
        list_finish(slot, KPB_ERR_SECURITY);
        return 0;
    }
    if (send_frame(fd, KPB_FRAME_STATUS, NULL, 0) != KPB_OK) {
        list_finish(slot, KPB_ERR_SYSTEM);
        return 0;
    }
    slot->buffer = malloc(LIST_REPLY_SIZE);
    if (!slot->buffer) {
        list_finish(slot, KPB_ERR_SYSTEM);
        return 0;
    }
    slot->received = 0;
    slot->state = LIST_ACTIVE;
    return 0;
}

/* Take whatever the broker has sent so far; finish the slot when it is
 * complete or has failed. */
static void
list_receive(list_slot *slot) {
    for (;;) {
        ssize_t count;
        if (slot->received >= LIST_REPLY_SIZE) break;
        count = recv(
            slot->fd, slot->buffer + slot->received,
            LIST_REPLY_SIZE - slot->received, MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            list_finish(slot, KPB_ERR_SYSTEM);
            return;
        }
        if (count == 0) {
            list_finish(slot, KPB_ERR_SYSTEM);
            return;
        }
        slot->received += (size_t)count;
        if (slot->received >= sizeof(kpb_frame_header)) {
            kpb_frame_header header;
            memcpy(&header, slot->buffer, sizeof header);
            if (ntohl(header.magic) != KPB_PROTOCOL_MAGIC ||
                ntohs(header.version) != KPB_PROTOCOL_VERSION ||
                ntohs(header.type) != KPB_FRAME_STATUS_REPLY ||
                ntohl(header.payload_size) != sizeof(kpb_wire_status)) {
                list_finish(slot, KPB_ERR_PROTOCOL);
                return;
            }
        }
    }
    {
        kpb_wire_status wire;
        kpb_result result;
        memcpy(&wire, slot->buffer + sizeof(kpb_frame_header), sizeof wire);
        slot->status = malloc(sizeof *slot->status);
        if (!slot->status) {
            list_finish(slot, KPB_ERR_SYSTEM);
            return;
        }
        result = wire_to_status(&wire, slot->status);
        if (result != KPB_OK) {
            free(slot->status);
            slot->status = NULL;
        }
        list_finish(slot, result);
    }
}

/* Query every session at once under one deadline.  The point is that the cost
 * of wedged brokers does not add up: asking N stopped brokers one after another
 * with a per-session bound costs N bounds, and a caller with its own timeout
 * (the engine gives `list` two seconds) would give up on the healthy sessions
 * behind the first wedged one.  Here every query is in flight together, so the
 * whole listing costs one deadline however many sessions are stuck. */
static void
list_query_all(
    const char *runtime_dir,
    list_slot *slots,
    size_t count,
    const struct timespec *deadline
) {
    struct pollfd descriptors[KPB_LIST_WINDOW];
    list_slot *active[KPB_LIST_WINDOW];
    size_t next = 0;
    size_t in_flight = 0;
    size_t finished = 0;
    while (finished < count) {
        size_t index;
        size_t polled = 0;
        long remaining;
        int ready;
        while (next < count && in_flight < KPB_LIST_WINDOW) {
            list_slot *slot = &slots[next];
            if (list_start(runtime_dir, slot, in_flight > 0) < 0) break;
            next++;
            if (slot->state == LIST_DONE) finished++;
            else in_flight++;
        }
        if (in_flight == 0) continue;
        remaining = millis_until(deadline);
        if (remaining <= 0) {
            for (index = 0; index < count; index++) {
                if (slots[index].state == LIST_ACTIVE) {
                    list_finish(&slots[index], KPB_ERR_TIMEOUT);
                } else if (slots[index].state == LIST_PENDING) {
                    slots[index].error = KPB_ERR_TIMEOUT;
                    slots[index].state = LIST_DONE;
                }
            }
            return;
        }
        for (index = 0; index < next; index++) {
            if (slots[index].state != LIST_ACTIVE) continue;
            active[polled] = &slots[index];
            descriptors[polled].fd = slots[index].fd;
            descriptors[polled].events = POLLIN;
            descriptors[polled].revents = 0;
            polled++;
        }
        ready = poll(descriptors, (nfds_t)polled, (int)(remaining > 1000L ? 1000L : remaining));
        if (ready < 0) {
            if (errno == EINTR) continue;
            for (index = 0; index < polled; index++) list_finish(active[index], KPB_ERR_SYSTEM);
            finished += polled;
            in_flight -= polled;
            continue;
        }
        for (index = 0; index < polled; index++) {
            if (!(descriptors[index].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            list_receive(active[index]);
            if (active[index]->state == LIST_DONE) {
                finished++;
                in_flight--;
            }
        }
    }
}

kpb_result
kpb_list_with_options(
    const char *runtime_dir,
    const kpb_list_options *options,
    kpb_list_entry_callback callback,
    void *data
) {
    session_paths paths;
    struct dirent *entry;
    DIR *directory;
    list_slot *slots = NULL;
    struct timespec deadline;
    size_t count = 0;
    size_t capacity = 0;
    size_t index;
    bool stopped = false;
    kpb_result result;
    if (!callback) return KPB_ERR_INVALID;
    /* ONE deadline for the whole call - the queries and the reaping lock. */
    deadline_from_timeout(
        &deadline, options ? options->timeout_millis : 0, KPB_DEFAULT_LIST_TIMEOUT_MILLIS);
    result = kpb_check_runtime(runtime_dir);
    if (result != KPB_OK) return result;
    result = build_paths(runtime_dir, NULL, &paths);
    if (result != KPB_OK) return result;
    result = check_directory(paths.sessions_dir);
    if (result == KPB_ERR_NOT_FOUND) return KPB_OK;
    if (result != KPB_OK) return result;
    directory = opendir(paths.sessions_dir);
    if (!directory) return errno == ENOENT ? KPB_OK : KPB_ERR_SYSTEM;
    while ((entry = readdir(directory))) {
        if (!valid_component(entry->d_name)) continue;
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2U : 32U;
            list_slot *replacement = realloc(slots, grown * sizeof *slots);
            if (!replacement) {
                closedir(directory);
                free(slots);
                return KPB_ERR_SYSTEM;
            }
            slots = replacement;
            capacity = grown;
        }
        memset(&slots[count], 0, sizeof slots[count]);
        slots[count].fd = -1;
        copy_string(slots[count].id, sizeof slots[count].id, entry->d_name);
        count++;
    }
    closedir(directory);

    list_query_all(runtime_dir, slots, count, &deadline);

    for (index = 0; index < count; index++) {
        list_slot *slot = &slots[index];
        kpb_list_entry item;
        if (slot->error == KPB_ERR_NOT_FOUND) {
            /* Nothing is listening in this directory.  If its metadata proves
             * the recorded broker is gone this is a corpse from an uncleanly
             * killed session, and this walk is the natural place to reap it -
             * otherwise it stays invisible forever while still blocking its
             * ID and costing every listing a connect() to a dead socket.
             * Proof and removal happen under the sessions-directory lock, so
             * a respawn that has just recreated this directory can never
             * lose its fresh files to the walk. */
            session_paths stale;
            if (build_paths(runtime_dir, slot->id, &stale) == KPB_OK) {
                /* Skipped, not waited for, when the lock cannot be had within the
                 * deadline: the session is still not listed (nothing is
                 * listening), and the next listing reaps it. */
                int lock_fd = lock_sessions_dir_until(&stale, &deadline);
                if (lock_fd >= 0) {
                    if (session_is_stale(&stale)) reap_stale_session(&stale);
                    close(lock_fd);
                }
            }
            continue;
        }
        if (stopped) continue;
        memset(&item, 0, sizeof item);
        copy_string(item.session_id, sizeof item.session_id, slot->id);
        item.error = slot->error;
        item.reachable = slot->error == KPB_OK && slot->status != NULL;
        if (item.reachable) item.status = *slot->status;
        if (callback(&item, data) != 0) stopped = true;
    }
    for (index = 0; index < count; index++) {
        if (slots[index].fd >= 0) close(slots[index].fd);
        free(slots[index].buffer);
        free(slots[index].status);
    }
    free(slots);
    return KPB_OK;
}

typedef struct {
    kpb_list_callback callback;
    void *data;
} list_adapter;

static int
adapt_list_entry(const kpb_list_entry *entry, void *opaque) {
    list_adapter *adapter = opaque;
    if (!entry->reachable) return 0;
    return adapter->callback(&entry->status, adapter->data);
}

kpb_result
kpb_list(const char *runtime_dir, kpb_list_callback callback, void *data) {
    list_adapter adapter;
    if (!callback) return KPB_ERR_INVALID;
    adapter.callback = callback;
    adapter.data = data;
    return kpb_list_with_options(runtime_dir, NULL, adapt_list_entry, &adapter);
}

/* --- the reaped archive ------------------------------------------------ */

static int
compare_reaped_entries(const void *left_opaque, const void *right_opaque) {
    const kpb_reaped_entry *left = left_opaque;
    const kpb_reaped_entry *right = right_opaque;
    if (left->started_millis != right->started_millis) {
        return left->started_millis < right->started_millis ? -1 : 1;
    }
    return strcmp(left->session_id, right->session_id);
}

/* Collect the archive, oldest-started first.  *out is malloc'd. */
static kpb_result
collect_reaped(const char *runtime_dir, kpb_reaped_entry **out, size_t *out_count) {
    session_paths paths;
    kpb_reaped_entry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;
    DIR *directory;
    kpb_result result = kpb_check_runtime(runtime_dir);
    *out = NULL;
    *out_count = 0;
    if (result != KPB_OK) return result;
    result = build_paths(runtime_dir, NULL, &paths);
    if (result != KPB_OK) return result;
    result = check_directory(paths.reaped_dir);
    if (result == KPB_ERR_NOT_FOUND) return KPB_OK;
    if (result != KPB_OK) return result;
    directory = opendir(paths.reaped_dir);
    if (!directory) return errno == ENOENT ? KPB_OK : KPB_ERR_SYSTEM;
    while ((entry = readdir(directory))) {
        kpb_reaped_entry item;
        struct stat journal;
        struct stat meta;
        char data[2048];
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2U : 16U;
            kpb_reaped_entry *replacement = realloc(entries, grown * sizeof *entries);
            if (!replacement) {
                closedir(directory);
                free(entries);
                return KPB_ERR_SYSTEM;
            }
            entries = replacement;
            capacity = grown;
        }
        memset(&item, 0, sizeof item);
        if (!parse_archive_name(entry->d_name, item.session_id, &item.started_millis)) continue;
        if (join_path(item.journal_path, sizeof item.journal_path, paths.reaped_dir, entry->d_name) != 0 ||
            lstat(item.journal_path, &journal) != 0 || !S_ISREG(journal.st_mode)) {
            continue;
        }
        item.journal_bytes = (uint64_t)journal.st_size;
        memcpy(item.meta_path, item.journal_path, sizeof item.meta_path);
        if (meta_path_for(item.meta_path, sizeof item.meta_path) == 0 &&
            lstat(item.meta_path, &meta) == 0 && S_ISREG(meta.st_mode)) {
            ssize_t size = read_small_file(item.meta_path, data, sizeof data);
            const char *line = data;
            if (size > 0) {
                while (line && *line) {
                    if (strncmp(line, "reaped_millis=", 14) == 0) {
                        item.reaped_millis = (uint64_t)strtoull(line + 14, NULL, 10);
                    }
                    line = strchr(line, '\n');
                    if (line) line++;
                }
            }
        } else {
            item.meta_path[0] = '\0';
        }
        entries[count++] = item;
    }
    closedir(directory);
    if (count > 1) qsort(entries, count, sizeof *entries, compare_reaped_entries);
    *out = entries;
    *out_count = count;
    return KPB_OK;
}

kpb_result
kpb_list_reaped(const char *runtime_dir, kpb_reaped_callback callback, void *data) {
    kpb_reaped_entry *entries;
    size_t count;
    size_t index;
    kpb_result result;
    if (!callback) return KPB_ERR_INVALID;
    result = collect_reaped(runtime_dir, &entries, &count);
    if (result != KPB_OK) return result;
    for (index = 0; index < count; index++) {
        if (callback(&entries[index], data) != 0) break;
    }
    free(entries);
    return KPB_OK;
}

kpb_result
kpb_reaped_path(
    const char *runtime_dir,
    const char *session_id,
    char *output,
    size_t capacity
) {
    kpb_reaped_entry *entries;
    size_t count;
    size_t index;
    kpb_result result;
    if (!output || capacity == 0) return KPB_ERR_INVALID;
    output[0] = '\0';
    if (kpb_validate_session_id(session_id) != KPB_OK) return KPB_ERR_INVALID;
    result = collect_reaped(runtime_dir, &entries, &count);
    if (result != KPB_OK) return result;
    result = KPB_ERR_NOT_FOUND;
    /* Sorted oldest-started first, so the newest is the last match. */
    for (index = count; index > 0; index--) {
        if (strcmp(entries[index - 1].session_id, session_id) != 0) continue;
        if (copy_string(output, capacity, entries[index - 1].journal_path) == 0) {
            result = KPB_OK;
        } else {
            result = KPB_ERR_BUFFER;
        }
        break;
    }
    free(entries);
    return result;
}
