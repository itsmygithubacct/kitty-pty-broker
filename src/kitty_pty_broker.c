#define _GNU_SOURCE

#include "kitty_pty_broker.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
    char runtime_dir[KPB_PATH_MAX];
    char sessions_dir[KPB_PATH_MAX];
    char session_dir[KPB_PATH_MAX];
    char socket_path[KPB_PATH_MAX];
    char journal_path[KPB_PATH_MAX];
    char metadata_path[KPB_PATH_MAX];
} session_paths;

/* An attached read-only observer.
 *
 * The read-write client is written with a blocking send, which is correct for
 * it: a frontend that stops reading should stop the shell.  An observer must
 * never have that power, so it gets a non-blocking socket and a bounded queue,
 * and is disconnected rather than buffered when it falls behind.  That is
 * cheap because a dropped observer can reattach and resume from the offset it
 * had reached.
 *
 * `generation` is bumped whenever a slot is released, so a poll result
 * captured before the slot was recycled can be recognised as stale. */
typedef struct {
    int fd;
    unsigned char *out;
    size_t out_offset;
    size_t out_size;
    size_t out_capacity;
    unsigned char in[sizeof(kpb_frame_header)];
    size_t in_size;
    uint32_t generation;
} observer_slot;

#define KPB_OBSERVER_QUEUE_LIMIT (2U * 1024U * 1024U)
/* Strictly below the queue limit: a fresh replay must never by itself fill the
 * queue and trip the drop policy, or observers would be disconnected at the
 * moment they attach. */
#define KPB_OBSERVER_REPLAY_MAX (1024U * 1024U)

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

static uint64_t
now_millis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
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

static kpb_result
build_paths(const char *runtime_dir, const char *session_id, session_paths *paths) {
    char resolved[KPB_PATH_MAX];
    if (!runtime_dir || runtime_dir[0] != '/' || !paths) return KPB_ERR_INVALID;
    if (strlen(runtime_dir) >= sizeof resolved) return KPB_ERR_INVALID;
    if (!realpath(runtime_dir, resolved)) {
        if (errno != ENOENT) return KPB_ERR_SYSTEM;
        if (copy_string(resolved, sizeof resolved, runtime_dir) != 0) return KPB_ERR_INVALID;
    }
    memset(paths, 0, sizeof *paths);
    if (copy_string(paths->runtime_dir, sizeof paths->runtime_dir, resolved) != 0 ||
        join_path(paths->sessions_dir, sizeof paths->sessions_dir, resolved, "sessions") != 0) {
        return KPB_ERR_INVALID;
    }
    if (!session_id) return KPB_OK;
    if (!valid_component(session_id)) return KPB_ERR_INVALID;
    if (join_path(paths->session_dir, sizeof paths->session_dir, paths->sessions_dir, session_id) != 0 ||
        join_path(paths->socket_path, sizeof paths->socket_path, paths->session_dir, "control.sock") != 0 ||
        join_path(paths->journal_path, sizeof paths->journal_path, paths->session_dir, "journal.bin") != 0 ||
        join_path(paths->metadata_path, sizeof paths->metadata_path, paths->session_dir, "metadata") != 0) {
        return KPB_ERR_INVALID;
    }
    if (strlen(paths->socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return KPB_ERR_INVALID;
    return KPB_OK;
}

static ssize_t
write_all_fd(int fd, const void *data, size_t size, bool socket_write) {
    const unsigned char *cursor = data;
    size_t written = 0;
    while (written < size) {
        ssize_t count = socket_write
            ? send(fd, cursor + written, size - written, MSG_NOSIGNAL)
            : write(fd, cursor + written, size - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) {
            errno = EPIPE;
            return -1;
        }
        written += (size_t)count;
    }
    return (ssize_t)written;
}

static ssize_t
read_all_fd(int fd, void *data, size_t size) {
    unsigned char *cursor = data;
    size_t received = 0;
    while (received < size) {
        ssize_t count = read(fd, cursor + received, size - received);
        if (count < 0) {
            if (errno == EINTR) continue;
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
    if (payload_size > KPB_PROTOCOL_MAX_PAYLOAD) return KPB_ERR_INVALID;
    header.magic = htonl(KPB_PROTOCOL_MAGIC);
    header.version = htons(KPB_PROTOCOL_VERSION);
    header.type = htons(type);
    header.payload_size = htonl(payload_size);
    if (write_all_fd(fd, &header, sizeof header, true) < 0) return KPB_ERR_SYSTEM;
    if (payload_size && write_all_fd(fd, payload, payload_size, true) < 0) return KPB_ERR_SYSTEM;
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
receive_frame(
    int fd,
    uint16_t *type,
    void *payload,
    size_t capacity,
    uint32_t *payload_size
) {
    kpb_frame_header header;
    uint32_t size;
    unsigned char discard[4096];
    if (read_all_fd(fd, &header, sizeof header) < 0) return KPB_ERR_SYSTEM;
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
            if (read_all_fd(fd, discard, chunk) < 0) return KPB_ERR_SYSTEM;
            left -= (uint32_t)chunk;
        }
        return KPB_ERR_BUFFER;
    }
    if (size && read_all_fd(fd, payload, size) < 0) return KPB_ERR_SYSTEM;
    return KPB_OK;
}

static kpb_result
connect_session(const char *runtime_dir, const char *session_id, int *fd_out) {
    session_paths paths;
    struct sockaddr_un address;
    int fd;
    kpb_result result = build_paths(runtime_dir, session_id, &paths);
    if (result != KPB_OK) return result;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return KPB_ERR_SYSTEM;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    copy_string(address.sun_path, sizeof address.sun_path, paths.socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return saved == ENOENT || saved == ECONNREFUSED ? KPB_ERR_NOT_FOUND : KPB_ERR_SYSTEM;
    }
    *fd_out = fd;
    return KPB_OK;
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

static int
write_metadata(server_state *server) {
    char temporary[KPB_PATH_MAX];
    char data[2048];
    int fd;
    int count;
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
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (write_all_fd(fd, data, (size_t)count, false) < 0 || fsync(fd) != 0) {
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

static void
close_client(server_state *server) {
    if (server->client_fd >= 0) close(server->client_fd);
    server->client_fd = -1;
}

static void
observer_close(server_state *server, size_t slot) {
    observer_slot *observer = &server->observers[slot];
    if (observer->fd >= 0) {
        close(observer->fd);
        if (server->observer_count) server->observer_count--;
    }
    free(observer->out);
    memset(observer, 0, sizeof *observer);
    observer->fd = -1;
    observer->generation = ++server->observer_generation;
}

static void
observers_close_all(server_state *server) {
    size_t slot;
    for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
        if (server->observers[slot].fd >= 0 || server->observers[slot].out) {
            observer_close(server, slot);
        }
    }
}

/* Queue one whole frame, or none of it.
 *
 * The capacity check precedes every append: a half-queued frame would
 * permanently desynchronise that observer's framing, which presents as garbled
 * output rather than as an error. */
static int
observer_enqueue(
    observer_slot *observer,
    uint16_t type,
    const void *payload,
    uint32_t size
) {
    kpb_frame_header header;
    size_t need = sizeof header + size;
    if (observer->out_offset && observer->out_offset == observer->out_size) {
        observer->out_offset = observer->out_size = 0;
    }
    if (observer->out_size - observer->out_offset + need > KPB_OBSERVER_QUEUE_LIMIT) {
        return -1;
    }
    if (observer->out_offset && observer->out_size + need > observer->out_capacity) {
        memmove(
            observer->out,
            observer->out + observer->out_offset,
            observer->out_size - observer->out_offset);
        observer->out_size -= observer->out_offset;
        observer->out_offset = 0;
    }
    if (observer->out_size + need > observer->out_capacity) {
        size_t capacity = observer->out_capacity ? observer->out_capacity : 65536;
        unsigned char *replacement;
        while (capacity < observer->out_size + need) capacity *= 2;
        if (capacity > KPB_OBSERVER_QUEUE_LIMIT) capacity = KPB_OBSERVER_QUEUE_LIMIT;
        if (observer->out_size + need > capacity) return -1;
        replacement = realloc(observer->out, capacity);
        if (!replacement) return -1;
        observer->out = replacement;
        observer->out_capacity = capacity;
    }
    header.magic = htonl(KPB_PROTOCOL_MAGIC);
    header.version = htons(KPB_PROTOCOL_VERSION);
    header.type = htons(type);
    header.payload_size = htonl(size);
    memcpy(observer->out + observer->out_size, &header, sizeof header);
    if (size) memcpy(observer->out + observer->out_size + sizeof header, payload, size);
    observer->out_size += need;
    return 0;
}

static int
observer_flush(observer_slot *observer) {
    while (observer->out_offset < observer->out_size) {
        ssize_t count = send(
            observer->fd,
            observer->out + observer->out_offset,
            observer->out_size - observer->out_offset,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (count == 0) return -1;
        observer->out_offset += (size_t)count;
    }
    observer->out_offset = observer->out_size = 0;
    return 0;
}

/* Refuse an observer with a reason it can read.  The pending queue is dropped
 * first so the short error always fits, then flushed once; a peer that cannot
 * even take that much is simply closed. */
static void
observer_refuse(server_state *server, size_t slot, const char *message) {
    observer_slot *observer = &server->observers[slot];
    observer->out_offset = observer->out_size = 0;
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
            write_all_fd(server->journal_fd, reset_sequence, sizeof reset_sequence - 1, false) < 0) {
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
    if (write_all_fd(server->journal_fd, data, size, false) < 0) return -1;
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

/* Slide the newest ``keep`` bytes to the front of the transcript and drop the
 * rest.  Rewriting in place (rather than renaming to a .1 file) keeps the
 * single long-lived writer attached to the same inode, matching how the rest
 * of the stack bounds its session logs. */
static int
rotate_transcript(server_state *server, uint64_t keep) {
    unsigned char buffer[KPB_IO_CHUNK];
    off_t read_offset;
    off_t write_offset = 0;
    if (keep > server->transcript_bytes) keep = server->transcript_bytes;
    read_offset = (off_t)(server->transcript_bytes - keep);
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
            if (ftruncate(server->transcript_fd, 0) != 0 ||
                lseek(server->transcript_fd, 0, SEEK_SET) < 0) {
                return -1;
            }
            server->transcript_bytes = 0;
            data += size - (size_t)keep;
            size = (size_t)keep;
        } else if (rotate_transcript(server, keep - size) != 0) {
            return -1;
        }
    }
    if (write_all_fd(server->transcript_fd, data, size, false) < 0) return -1;
    server->transcript_bytes += size;
    return 0;
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
    size_t index;
    if (server->transcript_fd < 0 || !size) return 0;
    if (server->transcript_graphics == KPB_TRANSCRIPT_GRAPHICS_KEEP) {
        return write_transcript(server, data, size);
    }
    for (index = 0; index < size; index++) {
        unsigned char byte = data[index];
        /* Guarantee room for the widest single-step emission: a marker, or a
         * held ESC _ prefix plus the byte that disproved it. */
        if (out_length + TRANSCRIPT_MARKER_MAX > sizeof out) {
            if (write_transcript(server, out, out_length) != 0) return -1;
            out_length = 0;
        }
        switch (server->transcript_scan) {
            case TRANSCRIPT_ESCAPE:
                if (byte == '_') {
                    server->transcript_scan = TRANSCRIPT_APC;
                    break;
                }
                out[out_length++] = 0x1b;
                server->transcript_scan = TRANSCRIPT_TEXT;
                /* fall through: reconsider this byte as ordinary text */
                __attribute__((fallthrough));
            case TRANSCRIPT_TEXT:
                if (byte == 0x1b) server->transcript_scan = TRANSCRIPT_ESCAPE;
                else out[out_length++] = byte;
                break;
            case TRANSCRIPT_APC:
                if (byte == 'G') {
                    server->transcript_scan = TRANSCRIPT_GRAPHICS;
                    server->transcript_elided = 3;
                    break;
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
            case TRANSCRIPT_GRAPHICS:
                server->transcript_elided++;
                if (byte == 0x1b) {
                    server->transcript_scan = TRANSCRIPT_GRAPHICS_ESCAPE;
                } else if (byte == 0x07) {
                    server->transcript_scan = TRANSCRIPT_TEXT;
                    out_length += format_elided_graphics(server, out + out_length);
                }
                break;
            case TRANSCRIPT_GRAPHICS_ESCAPE:
                server->transcript_elided++;
                if (byte == '\\') {
                    server->transcript_scan = TRANSCRIPT_TEXT;
                    out_length += format_elided_graphics(server, out + out_length);
                } else if (byte != 0x1b) {
                    server->transcript_scan = TRANSCRIPT_GRAPHICS;
                }
                break;
            default:
                server->transcript_scan = TRANSCRIPT_TEXT;
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

/* Send to whichever kind of peer this is: the read-write client writes
 * synchronously, an observer goes through its bounded queue. */
static kpb_result
peer_send(
    observer_slot *observer,
    int fd,
    uint16_t type,
    const void *payload,
    uint32_t size
) {
    if (observer) {
        return observer_enqueue(observer, type, payload, size) == 0
            ? KPB_OK : KPB_ERR_SYSTEM;
    }
    return send_frame(fd, type, payload, size);
}

/* Reads use pread so the journal's own append offset is never disturbed, and
 * clamp to plan->end rather than the file size: when journal_limit is smaller
 * than the reset sequence, journal_bytes deliberately exceeds the file. */
static int
replay_journal(
    server_state *server,
    observer_slot *observer,
    int client_fd,
    const replay_plan *plan
) {
    unsigned char buffer[KPB_IO_CHUNK];
    uint64_t at = plan->start;
    if (plan->truncated &&
        peer_send(observer, client_fd, KPB_FRAME_OUTPUT, "\033c", 2) != KPB_OK) {
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
        if (peer_send(
                observer, client_fd,
                KPB_FRAME_OUTPUT, buffer, (uint32_t)count) != KPB_OK) {
            return -1;
        }
        at += (uint64_t)count;
    }
    return peer_send(
        observer, client_fd, KPB_FRAME_REPLAY_DONE, NULL, 0) == KPB_OK ? 0 : -1;
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
    server->size.ws_row = ntohs(wire->rows);
    server->size.ws_col = ntohs(wire->columns);
    server->size.ws_xpixel = ntohs(wire->xpixel);
    server->size.ws_ypixel = ntohs(wire->ypixel);
    if (!server->size.ws_row) server->size.ws_row = 24;
    if (!server->size.ws_col) server->size.ws_col = 80;
    if (server->pty_fd >= 0) (void)ioctl(server->pty_fd, TIOCSWINSZ, &server->size);
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
    server->terminate_deadline = now_millis() + 1500;
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

static void
handle_v1_attach(server_state *server, int fd, const unsigned char *payload) {
    if (server->client_fd >= 0) {
        (void)send_error(fd, KPB_ERROR_ATTACHED);
        close(fd);
        return;
    }
    server->client_fd = fd;
    {
        replay_plan plan = {0, server->journal_bytes, false, false};
        if (replay_journal(server, NULL, fd, &plan) != 0) {
            close_client(server);
            return;
        }
    }
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

static int
set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
        server->client_fd = fd;
        if (send_frame(fd, KPB_FRAME_ATTACH_REPLY, &reply, sizeof reply) != KPB_OK ||
            replay_journal(server, NULL, fd, &plan) != 0) {
            close_client(server);
            return;
        }
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
        free(observer->out);
        memset(observer, 0, sizeof *observer);
        observer->fd = fd;
        observer->generation = ++server->observer_generation;
        server->observer_count++;
        /* Never apply_size here: an observer must not be able to resize the
         * pane, and the v1 path above calls apply_size unconditionally. */
        if (observer_enqueue(
                observer, KPB_FRAME_ATTACH_REPLY, &reply, sizeof reply) != 0 ||
            replay_journal(server, observer, -1, &plan) != 0 ||
            observer_flush(observer) != 0) {
            observer_close(server, slot);
        }
    }
}

static void
handle_new_connection(server_state *server) {
    unsigned char payload[sizeof(kpb_wire_status)];
    uint32_t payload_size = 0;
    uint16_t type = 0;
    int fd = accept4(server->listener_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    if (!peer_is_owner(fd)) {
        (void)send_error(fd, KPB_ERROR_UNAUTHORIZED);
        close(fd);
        return;
    }
    if (receive_frame(fd, &type, payload, sizeof payload, &payload_size) != KPB_OK) {
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
    uint32_t payload_size = 0;
    uint16_t type = 0;
    kpb_result result = receive_frame(
        server->client_fd, &type, payload, sizeof payload, &payload_size
    );
    if (result != KPB_OK) {
        close_client(server);
        return;
    }
    switch (type) {
        case KPB_FRAME_INPUT:
            if (queue_input(server, payload, payload_size) != 0) {
                (void)send_error(server->client_fd, KPB_ERROR_INPUT_LIMIT);
                close_client(server);
            }
            break;
        case KPB_FRAME_RESIZE:
            if (payload_size == sizeof(kpb_wire_winsize)) {
                apply_size(server, (const kpb_wire_winsize *)payload);
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
wait_status_to_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
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
    const uint64_t deadline = now_millis() + 200;
    while (now_millis() < deadline) {
        struct pollfd descriptor = {.fd = server->pty_fd, .events = POLLIN, .revents = 0};
        ssize_t count;
        int ready = poll(&descriptor, 1, 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) return;
        count = read(server->pty_fd, buffer, sizeof buffer);
        if (count <= 0) {
            if (count < 0 && errno == EINTR) continue;
            return;
        }
        if (append_journal(server, buffer, (size_t)count) != 0) return;
        if (append_transcript(server, buffer, (size_t)count) != 0) {
            close(server->transcript_fd);
            server->transcript_fd = -1;
        }
        if (server->client_fd >= 0 &&
            send_frame(server->client_fd, KPB_FRAME_OUTPUT, buffer, (uint32_t)count) != KPB_OK) {
            close_client(server);
        }
        observers_send(server, KPB_FRAME_OUTPUT, buffer, (uint32_t)count);
    }
}

/* Give observers a bounded chance to receive what is already queued, on the
 * same budget drain_pty uses.  A peer that will not read within it is closed
 * rather than allowed to delay teardown. */
static void
observers_drain(server_state *server) {
    const uint64_t deadline = now_millis() + 200;
    while (now_millis() < deadline) {
        struct pollfd descriptors[KPB_OBSERVER_MAX];
        size_t slots[KPB_OBSERVER_MAX];
        nfds_t pending = 0;
        size_t slot;
        int ready;
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            observer_slot *observer = &server->observers[slot];
            if (observer->fd < 0 || observer->out_size <= observer->out_offset) continue;
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

static int
server_loop(server_state *server) {
    unsigned char buffer[KPB_IO_CHUNK];
    int child_status = 0;
    bool child_exited = false;
    while (!child_exited) {
        struct pollfd descriptors[3 + KPB_OBSERVER_MAX];
        uint32_t generations[KPB_OBSERVER_MAX];
        size_t slot;
        int result;
        pid_t waited = waitpid(server->child_pid, &child_status, WNOHANG);
        if (waited == server->child_pid) child_exited = true;
        if (server->terminate_requested && now_millis() >= server->terminate_deadline) {
            signal_child_session(server->child_pid, SIGKILL);
            server->terminate_deadline = UINT64_MAX;
        }
        descriptors[0].fd = server->listener_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = server->pty_fd;
        descriptors[1].events = POLLIN | (server->input_size ? POLLOUT : 0);
        descriptors[1].revents = 0;
        descriptors[2].fd = server->client_fd;
        descriptors[2].events = server->client_fd >= 0 ? POLLIN : 0;
        descriptors[2].revents = 0;
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            observer_slot *observer = &server->observers[slot];
            descriptors[3 + slot].fd = observer->fd;
            descriptors[3 + slot].events = observer->fd >= 0
                ? (short)(POLLIN |
                    (observer->out_size > observer->out_offset ? POLLOUT : 0))
                : 0;
            descriptors[3 + slot].revents = 0;
            generations[slot] = observer->generation;
        }
        result = poll(descriptors, 3 + KPB_OBSERVER_MAX, child_exited ? 0 : 100);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (descriptors[0].revents & POLLIN) handle_new_connection(server);
        if (descriptors[2].revents & (POLLIN | POLLHUP | POLLERR)) {
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
        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            while (true) {
                ssize_t count = read(server->pty_fd, buffer, sizeof buffer);
                if (count > 0) {
                    if (append_journal(server, buffer, (size_t)count) != 0) return -1;
                    /* A transcript is best-effort: a full disk or a revoked
                     * directory must not take down a live shell. */
                    if (append_transcript(server, buffer, (size_t)count) != 0) {
                        close(server->transcript_fd);
                        server->transcript_fd = -1;
                    }
                    if (server->client_fd >= 0 &&
                        send_frame(server->client_fd, KPB_FRAME_OUTPUT, buffer, (uint32_t)count) != KPB_OK) {
                        close_client(server);
                    }
                    observers_send(server, KPB_FRAME_OUTPUT, buffer, (uint32_t)count);
                    if ((size_t)count < sizeof buffer) break;
                    continue;
                }
                if (count < 0 && (errno == EINTR)) continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)) break;
                break;
            }
        }
        if (descriptors[1].revents & POLLOUT) {
            if (flush_input(server) != 0) return -1;
        }
        /* Bound backlog latency to one tick.  This cannot spin: a queue that
         * survives the flush means the socket is full, and POLLOUT then simply
         * does not fire until it drains. */
        for (slot = 0; slot < KPB_OBSERVER_MAX; slot++) {
            observer_slot *observer = &server->observers[slot];
            if (observer->fd < 0 || observer->out_size <= observer->out_offset) continue;
            if (observer_flush(observer) != 0) observer_close(server, slot);
        }
    }
    drain_pty(server);
    {
        kpb_wire_exit wire;
        wire.wait_status = htonl(child_status);
        if (server->client_fd >= 0) {
            (void)send_frame(server->client_fd, KPB_FRAME_EXIT, &wire, sizeof wire);
        }
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

static void
reset_child_signals(void) {
    struct sigaction action;
    int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCHLD, SIGPIPE};
    size_t index;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (index = 0; index < sizeof signals / sizeof signals[0]; index++) {
        sigaction(signals[index], &action, NULL);
    }
}

static void
ignore_server_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGPIPE, &action, NULL);
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
    server.started_millis = now_millis();
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
    ignore_server_signals();
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
                lseek(server.transcript_fd, 0, SEEK_END) >= 0) {
                server.transcript_bytes = (uint64_t)transcript_status.st_size;
            } else {
                close(server.transcript_fd);
                server.transcript_fd = -1;
            }
        }
    }
    server.child_pid = forkpty(&server.pty_fd, NULL, NULL, &server.size);
    if (server.child_pid < 0) goto fail;
    if (server.child_pid == 0) {
        reset_child_signals();
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
    {
        int flags = fcntl(server.pty_fd, F_GETFL);
        if (flags >= 0) (void)fcntl(server.pty_fd, F_SETFL, flags | O_NONBLOCK);
    }
    if (write_metadata(&server) != 0) goto fail;
    memset(&ready, 0, sizeof ready);
    ready.broker_pid = getpid();
    ready.child_pid = server.child_pid;
    if (write_all_fd(ready_fd, &ready, sizeof ready, false) < 0) goto fail_after_ready;
    close(ready_fd);
    exit_code = server_loop(&server);
    cleanup_server(&server);
    return exit_code < 0 ? 255 : exit_code;

fail:
    memset(&ready, 0, sizeof ready);
    ready.error_number = errno ? errno : EIO;
    (void)write_all_fd(ready_fd, &ready, sizeof ready, false);
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
    session_id = options->session_id;
    if (!session_id || !*session_id) {
        result = kpb_generate_session_id(generated);
        if (result != KPB_OK) return result;
        session_id = generated;
    }
    if (kpb_validate_session_id(session_id) != KPB_OK) return KPB_ERR_INVALID;
    result = kpb_prepare_runtime(options->runtime_dir);
    if (result != KPB_OK) return result;
    result = build_paths(options->runtime_dir, session_id, &paths);
    if (result != KPB_OK) return result;
    if (lstat(paths.session_dir, &existing) == 0) return KPB_ERR_EXISTS;
    if (errno != ENOENT) return KPB_ERR_SYSTEM;
    if (mkdir(paths.session_dir, 0700) != 0) return errno == EEXIST ? KPB_ERR_EXISTS : KPB_ERR_SYSTEM;
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
kpb_attach(
    const char *runtime_dir,
    const char *session_id,
    unsigned short rows,
    unsigned short columns,
    unsigned short xpixel,
    unsigned short ypixel,
    kpb_connection *connection
) {
    kpb_wire_winsize size;
    int fd;
    kpb_result result;
    if (!connection) return KPB_ERR_INVALID;
    memset(connection, 0, sizeof *connection);
    connection->fd = -1;
    result = connect_session(runtime_dir, session_id, &fd);
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
    connection->fd = fd;
    copy_string(connection->session_id, sizeof connection->session_id, session_id);
    return KPB_OK;
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
    kpb_wire_attach request;
    kpb_wire_attach_reply reply;
    unsigned char staging[256];
    uint16_t type = 0;
    uint16_t code;
    uint32_t payload_size = 0;
    uint32_t flags;
    bool observing;
    int fd = -1;
    kpb_result outcome;

    if (!options || !connection) return KPB_ERR_INVALID;
    observing = options->mode == KPB_ATTACH_OBSERVE;
    if (options->mode != KPB_ATTACH_CONTROL && !observing) return KPB_ERR_INVALID;
    if ((observing || options->resume) && options->max_version < 2) {
        return KPB_ERR_INVALID;
    }
    if (result) memset(result, 0, sizeof *result);
    if (options->max_version < 2) {
        outcome = kpb_attach(
            runtime_dir, session_id,
            options->rows, options->columns,
            options->xpixel, options->ypixel, connection);
        if (outcome == KPB_OK && result) result->version = 1;
        return outcome;
    }

    memset(connection, 0, sizeof *connection);
    connection->fd = -1;
    outcome = connect_session(runtime_dir, session_id, &fd);
    if (outcome != KPB_OK) return outcome;

    memset(&request, 0, sizeof request);
    /* An observer sends zero dimensions: it must not be able to influence the
     * pane even if a future server were to read them. */
    request.rows = htons(observing ? 0 : options->rows);
    request.columns = htons(observing ? 0 : options->columns);
    request.xpixel = htons(observing ? 0 : options->xpixel);
    request.ypixel = htons(observing ? 0 : options->ypixel);
    request.version = htons((uint16_t)options->max_version);
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
    outcome = receive_frame(fd, &type, staging, sizeof staging, &payload_size);
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
    if (result) {
        result->version = ntohs(reply.version);
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
    result = receive_frame(connection->fd, &type, buffer, capacity, &payload_size);
    if (result != KPB_OK) return result;
    switch (type) {
        case KPB_FRAME_OUTPUT:
            event->type = KPB_EVENT_OUTPUT;
            event->size = payload_size;
            return KPB_OK;
        case KPB_FRAME_REPLAY_DONE:
            if (payload_size != 0) return KPB_ERR_PROTOCOL;
            event->type = KPB_EVENT_REPLAY_DONE;
            return KPB_OK;
        case KPB_FRAME_EXIT:
            if (payload_size != sizeof(kpb_wire_exit)) return KPB_ERR_PROTOCOL;
            event->type = KPB_EVENT_EXIT;
            event->exit_status = ntohl(((kpb_wire_exit *)buffer)->wait_status);
            return KPB_OK;
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
kpb_query_status(
    const char *runtime_dir,
    const char *session_id,
    kpb_status *status
) {
    kpb_wire_status wire;
    uint16_t type;
    uint32_t payload_size;
    int fd;
    kpb_result result;
    if (!status) return KPB_ERR_INVALID;
    result = connect_session(runtime_dir, session_id, &fd);
    if (result != KPB_OK) return result;
    result = send_frame(fd, KPB_FRAME_STATUS, NULL, 0);
    if (result == KPB_OK) {
        result = receive_frame(fd, &type, &wire, sizeof wire, &payload_size);
        if (result == KPB_OK &&
            (type != KPB_FRAME_STATUS_REPLY || payload_size != sizeof wire)) {
            result = KPB_ERR_PROTOCOL;
        }
    }
    close(fd);
    return result == KPB_OK ? wire_to_status(&wire, status) : result;
}

kpb_result
kpb_terminate(const char *runtime_dir, const char *session_id) {
    unsigned char payload[128];
    uint16_t type;
    uint32_t payload_size;
    int fd;
    kpb_result result = connect_session(runtime_dir, session_id, &fd);
    if (result != KPB_OK) return result;
    result = send_frame(fd, KPB_FRAME_TERMINATE, NULL, 0);
    if (result == KPB_OK) {
        result = receive_frame(fd, &type, payload, sizeof payload, &payload_size);
        if (result == KPB_OK && (type != KPB_FRAME_ACK || payload_size != 0)) {
            result = KPB_ERR_PROTOCOL;
        }
    }
    close(fd);
    return result;
}

kpb_result
kpb_list(const char *runtime_dir, kpb_list_callback callback, void *data) {
    session_paths paths;
    struct dirent *entry;
    DIR *directory;
    kpb_result result;
    if (!callback) return KPB_ERR_INVALID;
    result = build_paths(runtime_dir, NULL, &paths);
    if (result != KPB_OK) return result;
    directory = opendir(paths.sessions_dir);
    if (!directory) return errno == ENOENT ? KPB_OK : KPB_ERR_SYSTEM;
    while ((entry = readdir(directory))) {
        kpb_status status;
        if (!valid_component(entry->d_name)) continue;
        result = kpb_query_status(runtime_dir, entry->d_name, &status);
        if (result != KPB_OK) continue;
        if (callback(&status, data) != 0) break;
    }
    closedir(directory);
    return KPB_OK;
}
