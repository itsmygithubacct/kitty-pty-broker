#ifndef KITTY_PTY_BROKER_H
#define KITTY_PTY_BROKER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KPB_VERSION_MAJOR 0
#define KPB_VERSION_MINOR 4
#define KPB_VERSION_PATCH 0

/* Read-only observers may attach alongside the single read-write client. They
 * never claim the read-write slot, never reach the PTY, and are disconnected
 * rather than buffered when they fall behind. */
#define KPB_OBSERVER_MAX 8
#define KPB_PROTOCOL_VERSION_MAX 2

/* How long a spawn waits for the new broker to report that it is up, at least.
 * Generous: this wait covers forkpty and fsync on a possibly loaded machine, and
 * giving up early would turn a slow start into a failed one. */
#define KPB_SPAWN_READY_MILLIS 10000

/* The most history an observer is given on attach.  Beyond this the replay is
 * trimmed to the newest bytes, prefixed with a terminal reset, and reported as
 * truncated - the same contract the journal's own overflow uses. */
#define KPB_OBSERVER_REPLAY_MAX (1024ULL * 1024ULL)

#define KPB_SESSION_ID_MAX 64
#define KPB_PATH_MAX 4096
#define KPB_COMMAND_MAX 512
#define KPB_IO_CHUNK (32U * 1024U)
#define KPB_DEFAULT_JOURNAL_LIMIT (64ULL * 1024ULL * 1024ULL)
#define KPB_DEFAULT_TRANSCRIPT_LIMIT (8ULL * 1024ULL * 1024ULL)

/* Every client operation that waits on a broker is bounded.  A broker that is
 * stopped, wedged, or swapped out never answers, and an unbounded wait turns
 * that one session into a hang for everything that asks about it.  Timeouts
 * are in milliseconds; a value <= 0 selects the default. */
#define KPB_DEFAULT_TIMEOUT_MILLIS 2000
/* The overall deadline of a whole list, shared by every session it queries. */
#define KPB_DEFAULT_LIST_TIMEOUT_MILLIS 1000

/* Once any byte of a frame has arrived, kpb_receive requires the rest within
 * this, the same budget the broker gives an attached client's frames. */
#define KPB_FRAME_TIMEOUT_MILLIS 2000

/* The longest socket path a Unix domain socket can bind, excluding the NUL. */
#define KPB_SOCKET_PATH_LIMIT 107

/* Bounds on RUNTIME/reaped/, the archive of journals from reaped sessions.
 * Overridable through the environment of the process that does the reaping
 * (KITTY_PTY_BROKER_REAPED_MAX_BYTES, KITTY_PTY_BROKER_REAPED_MAX_FILES). */
#define KPB_DEFAULT_REAPED_MAX_BYTES (256ULL * 1024ULL * 1024ULL)
#define KPB_DEFAULT_REAPED_MAX_FILES 64U

/* The replay journal and the transcript are deliberately different things.
 * The journal is a bounded buffer whose only job is to repaint a reattaching
 * client, so it discards all history when it overflows.  A transcript is a
 * durable session log: it keeps the newest bytes and never resets the screen. */
typedef enum {
    KPB_TRANSCRIPT_GRAPHICS_ELIDE = 0,
    KPB_TRANSCRIPT_GRAPHICS_KEEP = 1
} kpb_transcript_graphics;

typedef enum {
    KPB_OK = 0,
    KPB_ERR_INVALID = 1,
    KPB_ERR_SYSTEM = 2,
    KPB_ERR_SECURITY = 3,
    KPB_ERR_EXISTS = 4,
    KPB_ERR_NOT_FOUND = 5,
    KPB_ERR_BUSY = 6,
    KPB_ERR_PROTOCOL = 7,
    KPB_ERR_BUFFER = 8,
    KPB_ERR_CHILD = 9,
    /* Appended after KPB_ERR_CHILD.  Neither is ever sent on the wire: a broker
     * reports only the values above, and a client treats anything else it is
     * handed as a protocol error. */
    KPB_ERR_TIMEOUT = 10,
    KPB_ERR_NAME_TOO_LONG = 11,
    /* Results of kpb_terminate_expect only, and also never in a wire result
     * code.  MISMATCH: the broker answered and is a different session than the
     * one the caller meant; nothing was done.  UNSUPPORTED: the broker predates
     * identity-bound terminate and could not check; nothing was done. */
    KPB_ERR_MISMATCH = 12,
    KPB_ERR_UNSUPPORTED = 13,
    /* kpb_terminate_timeout / kpb_terminate_expect only: the deadline passed
     * while still CONNECTING, so the request was never sent and nothing was or
     * will be done.  Contrast KPB_ERR_TIMEOUT, which for those calls means the
     * request WAS sent and may still be acted on. */
    KPB_ERR_NOT_SENT = 14
} kpb_result;

typedef struct {
    const char *runtime_dir;
    const char *session_id;
    const char *cwd;
    char *const *argv;
    uint64_t journal_limit;
    const char *transcript_path;
    uint64_t transcript_limit;
    int transcript_graphics;
    unsigned short rows;
    unsigned short columns;
    unsigned short xpixel;
    unsigned short ypixel;
} kpb_spawn_options;

typedef struct {
    int fd;
    char session_id[KPB_SESSION_ID_MAX + 1];
} kpb_connection;

typedef struct {
    char session_id[KPB_SESSION_ID_MAX + 1];
    pid_t broker_pid;
    pid_t child_pid;
    pid_t foreground_pgrp;
    uint64_t started_millis;
    uint64_t journal_bytes;
    uint64_t journal_epoch;
    int attached;
    int replay_complete;
    unsigned short rows;
    unsigned short columns;
    char cwd[KPB_PATH_MAX];
    char command[KPB_COMMAND_MAX];
} kpb_status;

typedef enum {
    KPB_EVENT_OUTPUT = 1,
    KPB_EVENT_REPLAY_DONE = 2,
    KPB_EVENT_EXIT = 3,
    KPB_EVENT_ERROR = 4,
    /* A screen reset to apply before what follows, carried in the buffer.
     * Distinct from KPB_EVENT_OUTPUT because it occupies no journal position:
     * a consumer tracking its offset must write these bytes and NOT count
     * them. */
    KPB_EVENT_RESET = 5
} kpb_event_type;

typedef struct {
    kpb_event_type type;
    size_t size;
    int exit_status;
} kpb_event;

typedef enum {
    KPB_ATTACH_CONTROL = 0,
    KPB_ATTACH_OBSERVE = 1
} kpb_attach_mode;

/* Options for a session-protocol-2 attach.  Initialize with
 * kpb_attach_options_init(), which selects control mode at the highest
 * supported version.  Setting max_version below 2 makes the call emit an
 * ordinary v1 attach, which is what keeps this usable against a broker from a
 * previous build; observe and resume then become invalid rather than silently
 * degrading. */
typedef struct {
    unsigned short rows;
    unsigned short columns;
    unsigned short xpixel;
    unsigned short ypixel;
    int mode;
    int max_version;
    int resume;
    uint64_t resume_epoch;
    uint64_t resume_offset;
} kpb_attach_options;

/* What the broker decided.  journal_offset is the stream position of the first
 * byte that follows, so a caller can carry it forward as a cursor by adding
 * the size of every output event it then receives. */
typedef struct {
    int version;
    int resumed;
    int truncated;
    int replay_complete;
    uint64_t journal_epoch;
    uint64_t journal_offset;
} kpb_attach_result;

typedef int (*kpb_list_callback)(const kpb_status *status, void *data);

/* One session found by kpb_list_with_options.  A session that is listening and
 * answered is reachable.  A session whose status could not be had for any
 * reason other than "nothing is listening" - it did not answer in time, the
 * peer failed the owner check, it spoke garbage - is reported with
 * reachable == 0 and the reason in `error`; `status` is then meaningless.  A
 * directory with nothing listening is a corpse, not a session, and is never
 * reported (it is reaped when its broker is provably gone). */
typedef struct {
    char session_id[KPB_SESSION_ID_MAX + 1];
    int reachable;
    kpb_result error;
    kpb_status status;
} kpb_list_entry;

typedef struct {
    /* The overall deadline for the whole listing, shared by all sessions,
     * which are queried concurrently.  <= 0 selects
     * KPB_DEFAULT_LIST_TIMEOUT_MILLIS. */
    int timeout_millis;
} kpb_list_options;

typedef int (*kpb_list_entry_callback)(const kpb_list_entry *entry, void *data);

/* A journal kept when its session was reaped (see kpb_list_reaped). */
typedef struct {
    char session_id[KPB_SESSION_ID_MAX + 1];
    uint64_t started_millis;
    /* 0 when the .meta file is absent or does not record it. */
    uint64_t reaped_millis;
    uint64_t journal_bytes;
    char journal_path[KPB_PATH_MAX];
    /* Empty when there is no .meta file. */
    char meta_path[KPB_PATH_MAX];
} kpb_reaped_entry;

typedef int (*kpb_reaped_callback)(const kpb_reaped_entry *entry, void *data);

void kpb_spawn_options_init(kpb_spawn_options *options);
void kpb_attach_options_init(kpb_attach_options *options);
const char *kpb_result_string(kpb_result result);
int kpb_protocol_version(void);
int kpb_protocol_version_max(void);

kpb_result kpb_generate_session_id(char output[KPB_SESSION_ID_MAX + 1]);
kpb_result kpb_validate_session_id(const char *session_id);
kpb_result kpb_prepare_runtime(const char *runtime_dir);

/* Caller descriptors that survive normal FD_CLOEXEC handling remain available
 * to the spawned command, while the persistent broker closes its copies.  The
 * command starts with an empty signal mask; dispositions changed by the broker
 * for its own lifecycle are restored to their conventional defaults. */
/* May return KPB_ERR_TIMEOUT: the sessions lock could not be had within
 * KPB_DEFAULT_TIMEOUT_MILLIS, or the new broker did not report ready (see
 * kpb_spawn_timeout).  Either way no session was started. */
kpb_result kpb_spawn(const kpb_spawn_options *options, kpb_status *status);
/* As kpb_spawn, with an explicit bound (<= 0: KPB_DEFAULT_TIMEOUT_MILLIS) on
 * waiting for the sessions-directory lock.  Both forms can return
 * KPB_ERR_TIMEOUT: either the lock could not be had in time (nothing was
 * created), or the new broker did not report ready within the larger of that
 * bound and KPB_SPAWN_READY_MILLIS.  In the second case nobody is waiting any
 * more, so the broker tears itself down instead of coming up unobserved - a
 * timeout means no session was started. */
kpb_result kpb_spawn_timeout(
    const kpb_spawn_options *options,
    kpb_status *status,
    int timeout_millis
);
kpb_result kpb_attach(
    const char *runtime_dir,
    const char *session_id,
    unsigned short rows,
    unsigned short columns,
    unsigned short xpixel,
    unsigned short ypixel,
    kpb_connection *connection
);
kpb_result kpb_attach_with_options(
    const char *runtime_dir,
    const char *session_id,
    const kpb_attach_options *options,
    kpb_connection *connection,
    kpb_attach_result *result
);
/* The same attaches with an explicit bound on connecting and on the broker's
 * first answer.  A plain version-1 attach has no reply frame, so the answer it
 * waits for is the first frame of the replay, which a live broker sends
 * immediately; nothing is consumed.  Returns KPB_ERR_TIMEOUT when it expires.
 * kpb_attach and kpb_attach_with_options use KPB_DEFAULT_TIMEOUT_MILLIS. */
kpb_result kpb_attach_timeout(
    const char *runtime_dir,
    const char *session_id,
    unsigned short rows,
    unsigned short columns,
    unsigned short xpixel,
    unsigned short ypixel,
    kpb_connection *connection,
    int timeout_millis
);
kpb_result kpb_attach_with_options_timeout(
    const char *runtime_dir,
    const char *session_id,
    const kpb_attach_options *options,
    kpb_connection *connection,
    kpb_attach_result *result,
    int timeout_millis
);
kpb_result kpb_observe(
    const char *runtime_dir,
    const char *session_id,
    kpb_connection *connection,
    kpb_attach_result *result
);
kpb_result kpb_send_input(kpb_connection *connection, const void *data, size_t size);
kpb_result kpb_resize(
    kpb_connection *connection,
    unsigned short rows,
    unsigned short columns,
    unsigned short xpixel,
    unsigned short ypixel
);
/* Receive one event.  The broker emits OUTPUT payloads of up to KPB_IO_CHUNK
 * bytes, so a buffer of at least KPB_IO_CHUNK is the capacity that can never
 * lose data; smaller buffers are accepted (down to the 4 bytes an EXIT frame
 * needs) for callers that only expect small frames.  A frame larger than
 * `capacity` is consumed and discarded so the stream's framing survives, and
 * the call reports the loss rather than hiding it: it returns KPB_ERR_BUFFER
 * with event->size set to the discarded payload's size and event->type to the
 * event it would have delivered (0 for types that carry no payload).  Skipped
 * output is journal content, so a protocol-2 caller that keeps a cursor (see
 * kpb_attach_result) can recover the bytes by reattaching with resume from
 * that cursor, which the failed call does not advance.
 *
 * Waiting for a frame to BEGIN is unbounded - this is the live stream, and a
 * quiet pane is not a fault - but once any byte of a frame has arrived the rest
 * of it must follow within KPB_FRAME_TIMEOUT_MILLIS.  If it does not, the call
 * returns KPB_ERR_TIMEOUT; the stream is then out of step and the connection
 * should be dropped (reattach with resume to continue).  A non-blocking
 * descriptor still fails at once when no data is waiting. */
kpb_result kpb_receive(
    kpb_connection *connection,
    void *buffer,
    size_t capacity,
    kpb_event *event
);
void kpb_detach(kpb_connection *connection);

kpb_result kpb_query_status(
    const char *runtime_dir,
    const char *session_id,
    kpb_status *status
);
kpb_result kpb_terminate(const char *runtime_dir, const char *session_id);
/* Bounded forms (KPB_ERR_TIMEOUT on expiry); the plain forms above use
 * KPB_DEFAULT_TIMEOUT_MILLIS.  A terminate that times out after its request
 * was sent (KPB_ERR_TIMEOUT) may still be acted on by the broker later; one
 * that times out while still connecting returns KPB_ERR_NOT_SENT instead, and
 * nothing was done. */
kpb_result kpb_query_status_timeout(
    const char *runtime_dir,
    const char *session_id,
    kpb_status *status,
    int timeout_millis
);
kpb_result kpb_terminate_timeout(
    const char *runtime_dir,
    const char *session_id,
    int timeout_millis
);
/* Terminate the session only if it is the one the caller saw.  A status query
 * followed by an unconditional kpb_terminate cannot make that guarantee: the
 * session can be replaced under the same ID between the two, and the
 * replacement is the one that dies.  This sends the status's `started_millis`
 * WITH the request, and the broker - which owns exactly one session - compares
 * it with its own and acts only on a match, in one step.
 *
 *   KPB_OK            the identity matched; the broker is ending the session
 *   KPB_ERR_MISMATCH  a different session now answers to that ID; nothing was done
 *   KPB_ERR_UNSUPPORTED  the broker is from a build that predates this request
 *                     and answered "invalid request"; nothing was done.  The
 *                     library does NOT retry unconditionally - that decision,
 *                     and the risk, belong to the caller (kpb_terminate).
 *   KPB_ERR_TIMEOUT   as for kpb_terminate_timeout: the request may still be
 *                     acted on, and if it is, it is still identity-checked.
 *
 *   KPB_ERR_NOT_SENT  the deadline passed while still connecting: nothing was
 *                     sent, so nothing was or will be done (safe to retry).
 *
 * `expected_started_millis` is compared as given: 0 is not "no expectation". */
kpb_result kpb_terminate_expect(
    const char *runtime_dir,
    const char *session_id,
    uint64_t expected_started_millis,
    int timeout_millis
);

/* Calls `callback` for every reachable session, in directory order, until it
 * returns nonzero.  All sessions are queried concurrently under one overall
 * deadline (KPB_DEFAULT_LIST_TIMEOUT_MILLIS), so any number of wedged brokers
 * costs one deadline, not one each; sessions that do not answer in time are
 * left out.  Returns KPB_ERR_NOT_FOUND when `runtime_dir` does not exist (a
 * runtime that exists with no sessions is an empty listing) and
 * KPB_ERR_SECURITY when it is a symlink, not a directory, or not owned by the
 * caller.  Corpses whose broker is provably gone are reaped on the way. */
kpb_result kpb_list(
    const char *runtime_dir,
    kpb_list_callback callback,
    void *data
);
/* As kpb_list, but options select the deadline and unreachable sessions are
 * reported through the callback instead of being left out.  `options` may be
 * NULL for the defaults. */
kpb_result kpb_list_with_options(
    const char *runtime_dir,
    const kpb_list_options *options,
    kpb_list_entry_callback callback,
    void *data
);

/* KPB_OK when `runtime_dir` is an existing real directory owned by the
 * caller; KPB_ERR_NOT_FOUND when it does not exist; KPB_ERR_SECURITY when it
 * is a symlink, not a directory, or owned by someone else.  Read-only: unlike
 * kpb_prepare_runtime it creates and changes nothing. */
kpb_result kpb_check_runtime(const char *runtime_dir);

/* The socket path a session would use, written to `output` (NUL-terminated,
 * and left empty if it does not fit `capacity`).  Returns
 * KPB_ERR_NAME_TOO_LONG when it exceeds KPB_SOCKET_PATH_LIMIT bytes, in which
 * case no session can live there; the path is still written so a caller can
 * say what it was.  Creates nothing. */
kpb_result kpb_session_socket_path(
    const char *runtime_dir,
    const char *session_id,
    char *output,
    size_t capacity
);

/* The current directory of the session's command, read from /proc/CHILD/cwd.
 * kpb_status.cwd is the directory the session STARTED in and never changes.
 * Returns KPB_ERR_NOT_FOUND where /proc is not available, the process is gone,
 * or it belongs to another user.  This runs in the caller, not the broker, and
 * adds nothing to the wire. */
kpb_result kpb_read_cwd_now(pid_t child_pid, char *output, size_t capacity);
/* As kpb_read_cwd_now, and says why there is no answer when the directory the
 * command is in has been REMOVED: *deleted is set and the result is
 * KPB_ERR_NOT_FOUND with `output` empty (never the kernel's "/path (deleted)"
 * link text, which is not a path).  `deleted` may be NULL.  Note that
 * `child_pid` is whatever the broker reported; it is trusted exactly as far as a
 * same-user broker is. */
kpb_result kpb_read_cwd_now_ex(pid_t child_pid, char *output, size_t capacity, int *deleted);

/* The identity of a broker process, read by the caller from /proc, so a later
 * request can be bound to the exact session that was seen (a pid or an ID alone
 * can be reused).  kpb_read_boot_id writes this machine's boot id (at most 63
 * characters); kpb_read_start_ticks reads field 22 of /proc/PID/stat.  Both
 * return KPB_ERR_NOT_FOUND where /proc is unavailable or the process is gone.
 * They describe the machine and /proc the CALLER sees. */
kpb_result kpb_read_boot_id(char output[64]);
kpb_result kpb_read_start_ticks(pid_t pid, uint64_t *ticks);

/* Journals of sessions whose broker died uncleanly.  Reaping such a session
 * moves its journal to RUNTIME/reaped/ID.STARTED_MILLIS.journal beside an
 * ID.STARTED_MILLIS.meta file instead of deleting it, and bounds that
 * directory (KPB_DEFAULT_REAPED_MAX_BYTES, KPB_DEFAULT_REAPED_MAX_FILES; oldest
 * evicted first).  kpb_list_reaped reports them oldest-started first; a missing
 * reaped/ directory is an empty listing.  kpb_reaped_path writes the journal
 * path of the newest archive for `session_id`, or returns KPB_ERR_NOT_FOUND. */
kpb_result kpb_list_reaped(
    const char *runtime_dir,
    kpb_reaped_callback callback,
    void *data
);
kpb_result kpb_reaped_path(
    const char *runtime_dir,
    const char *session_id,
    char *output,
    size_t capacity
);

#ifdef __cplusplus
}
#endif

#endif
