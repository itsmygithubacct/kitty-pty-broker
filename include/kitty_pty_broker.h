#ifndef KITTY_PTY_BROKER_H
#define KITTY_PTY_BROKER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KPB_VERSION_MAJOR 0
#define KPB_VERSION_MINOR 3
#define KPB_VERSION_PATCH 0

/* Read-only observers may attach alongside the single read-write client. They
 * never claim the read-write slot, never reach the PTY, and are disconnected
 * rather than buffered when they fall behind. */
#define KPB_OBSERVER_MAX 8
#define KPB_PROTOCOL_VERSION_MAX 2

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
    KPB_ERR_CHILD = 9
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
    KPB_EVENT_ERROR = 4
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

void kpb_spawn_options_init(kpb_spawn_options *options);
void kpb_attach_options_init(kpb_attach_options *options);
const char *kpb_result_string(kpb_result result);
int kpb_protocol_version(void);
int kpb_protocol_version_max(void);

kpb_result kpb_generate_session_id(char output[KPB_SESSION_ID_MAX + 1]);
kpb_result kpb_validate_session_id(const char *session_id);
kpb_result kpb_prepare_runtime(const char *runtime_dir);

kpb_result kpb_spawn(const kpb_spawn_options *options, kpb_status *status);
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
kpb_result kpb_list(
    const char *runtime_dir,
    kpb_list_callback callback,
    void *data
);

#ifdef __cplusplus
}
#endif

#endif
