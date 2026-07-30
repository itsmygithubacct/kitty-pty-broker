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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *current_test = "(startup)";

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: [%s] check failed: %s (errno=%d %s)\n", \
                __FILE__, __LINE__, current_test, #condition, \
                errno, strerror(errno)); \
        exit(1); \
    } \
} while (0)

/* Name the running test so a timeout inside a shared helper is attributable. */
#define RUN(test) do { current_test = #test; test(); } while (0)

static char runtime_dir[] = "/tmp/kitty-pty-broker-test.XXXXXX";
static char test_program_path[KPB_PATH_MAX];
static const char *test_program;

static void
wait_readable(int fd) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
    int result;
    do {
        result = poll(&descriptor, 1, 3000);
    } while (result < 0 && errno == EINTR);
    CHECK(result > 0);
}

static size_t
read_until_replay_done(kpb_connection *connection, unsigned char *output, size_t capacity) {
    unsigned char buffer[KPB_IO_CHUNK];
    size_t used = 0;
    while (true) {
        kpb_event event;
        kpb_result result;
        wait_readable(connection->fd);
        result = kpb_receive(connection, buffer, sizeof buffer, &event);
        CHECK(result == KPB_OK);
        if (event.type == KPB_EVENT_REPLAY_DONE) return used;
        CHECK(event.type == KPB_EVENT_OUTPUT);
        CHECK(used + event.size <= capacity);
        memcpy(output + used, buffer, event.size);
        used += event.size;
    }
}

static int
read_until_exit(kpb_connection *connection, unsigned char *output, size_t *used, size_t capacity) {
    unsigned char buffer[KPB_IO_CHUNK];
    while (true) {
        kpb_event event;
        kpb_result result;
        wait_readable(connection->fd);
        result = kpb_receive(connection, buffer, sizeof buffer, &event);
        CHECK(result == KPB_OK);
        if (event.type == KPB_EVENT_EXIT) return event.exit_status;
        if (event.type == KPB_EVENT_REPLAY_DONE) continue;
        CHECK(event.type == KPB_EVENT_OUTPUT);
        CHECK(*used + event.size <= capacity);
        memcpy(output + *used, buffer, event.size);
        *used += event.size;
    }
}

static void
test_ids(void) {
    char id[KPB_SESSION_ID_MAX + 1];
    CHECK(kpb_generate_session_id(id) == KPB_OK);
    CHECK(strlen(id) == 32);
    CHECK(kpb_validate_session_id(id) == KPB_OK);
    CHECK(kpb_validate_session_id("../bad") == KPB_ERR_INVALID);
    CHECK(kpb_validate_session_id("") == KPB_ERR_INVALID);
}

static void
test_spawn_detach_replay_and_exit(void) {
    char *command[] = {
        "/bin/sh", "-c",
        "printf '\\033_Gi=7,a=q;GRAPHICS_OK\\033\\\\before:'; "
        "IFS= read -r first; printf 'one=%s:' \"$first\"; "
        "IFS= read -r second; printf 'two=%s\\n' \"$second\"",
        NULL
    };
    kpb_spawn_options options;
    kpb_connection first;
    kpb_connection second;
    kpb_status status;
    unsigned char output[16384];
    size_t used;
    int wait_status;

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "replay";
    options.cwd = "/tmp";
    options.argv = command;
    options.rows = 30;
    options.columns = 100;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    CHECK(status.child_pid > 0);
    CHECK(strcmp(status.session_id, "replay") == 0);

    CHECK(kpb_attach(runtime_dir, "replay", 30, 100, 800, 600, &first) == KPB_OK);
    used = read_until_replay_done(&first, output, sizeof output);
    while (!memmem(output, used, "before:", 7)) {
        kpb_event event;
        wait_readable(first.fd);
        CHECK(kpb_receive(&first, output + used, sizeof output - used, &event) == KPB_OK);
        CHECK(event.type == KPB_EVENT_OUTPUT);
        used += event.size;
    }
    {
        static const char graphics[] = "\033_Gi=7,a=q;GRAPHICS_OK\033\\";
        CHECK(memmem(output, used, graphics, sizeof graphics - 1) != NULL);
    }
    CHECK(kpb_send_input(&first, "alpha\n", 6) == KPB_OK);
    kpb_detach(&first);

    usleep(100000);
    CHECK(kpb_query_status(runtime_dir, "replay", &status) == KPB_OK);
    CHECK(!status.attached);
    CHECK(kill(status.broker_pid, 0) == 0);
    CHECK(kill(status.child_pid, 0) == 0);

    CHECK(kpb_attach(runtime_dir, "replay", 40, 120, 1000, 700, &second) == KPB_OK);
    used = read_until_replay_done(&second, output, sizeof output);
    CHECK(memmem(output, used, "before:", 7) != NULL);
    while (!memmem(output, used, "one=alpha:", 10)) {
        kpb_event event;
        wait_readable(second.fd);
        CHECK(kpb_receive(&second, output + used, sizeof output - used, &event) == KPB_OK);
        CHECK(event.type == KPB_EVENT_OUTPUT);
        used += event.size;
    }
    CHECK(kpb_send_input(&second, "omega\n", 6) == KPB_OK);
    wait_status = read_until_exit(&second, output, &used, sizeof output);
    CHECK(WIFEXITED(wait_status));
    CHECK(WEXITSTATUS(wait_status) == 0);
    CHECK(memmem(output, used, "two=omega", 9) != NULL);
    kpb_detach(&second);
}

static void
test_terminate(void) {
    char *command[] = {"/bin/sh", "-c", "trap '' TERM; while :; do sleep 1; done", NULL};
    kpb_spawn_options options;
    kpb_status status;
    int attempts;
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "terminate";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    CHECK(kpb_terminate(runtime_dir, "terminate") == KPB_OK);
    for (attempts = 0; attempts < 40; attempts++) {
        if (kpb_query_status(runtime_dir, "terminate", &status) == KPB_ERR_NOT_FOUND) return;
        usleep(100000);
    }
    CHECK(!"terminated session disappeared");
}

static size_t
read_pty_until(
    int fd,
    unsigned char *output,
    size_t used,
    size_t capacity,
    const char *needle
) {
    size_t needle_size = strlen(needle);
    while (!memmem(output, used, needle, needle_size)) {
        ssize_t count;
        wait_readable(fd);
        count = read(fd, output + used, capacity - used);
        CHECK(count > 0);
        used += (size_t)count;
        CHECK(used < capacity);
    }
    return used;
}

static void
test_tui(void) {
    static const char injected[] = "\033]KPB_INJECT";
    char *command[] = {
        "/bin/sh", "-c",
        "trap '' TERM; while :; do sleep 1; done # \033]KPB_INJECT",
        NULL
    };
    char cli_path[KPB_PATH_MAX];
    const char *slash;
    size_t directory_size;
    kpb_spawn_options options;
    kpb_status status;
    struct winsize size = {.ws_row = 24, .ws_col = 100};
    unsigned char output[65536];
    size_t used = 0;
    int master;
    int wait_status;
    pid_t child;

    slash = strrchr(test_program_path, '/');
    CHECK(slash != NULL);
    directory_size = (size_t)(slash - test_program_path);
    CHECK(directory_size + sizeof "/kitty-pty-broker" < sizeof cli_path);
    memcpy(cli_path, test_program_path, directory_size);
    memcpy(
        cli_path + directory_size,
        "/kitty-pty-broker",
        sizeof "/kitty-pty-broker"
    );

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "tui-session";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);

    child = forkpty(&master, NULL, NULL, &size);
    CHECK(child >= 0);
    if (child == 0) {
        execl(
            cli_path,
            cli_path,
            "--runtime-dir",
            runtime_dir,
            "tui",
            (char *)NULL
        );
        _exit(127);
    }
    used = read_pty_until(
        master, output, used, sizeof output, "KILIX TUI"
    );
    used = read_pty_until(
        master, output, used, sizeof output, "▶1 Sessions"
    );
    used = read_pty_until(
        master, output, used, sizeof output, "tui-session"
    );
    CHECK(memmem(output, used, "─", sizeof "─" - 1U) != NULL);
    CHECK(memmem(output, used, injected, sizeof injected - 1U) == NULL);

    /* A split arrow-key escape sequence must not be mistaken for quit. */
    CHECK(write(master, "\033", 1) == 1);
    usleep(10000);
    CHECK(write(master, "[B", 2) == 2);
    CHECK(write(master, "x", 1) == 1);
    used = read_pty_until(
        master, output, used, sizeof output, "Terminate tui-session"
    );
    CHECK(write(master, "n", 1) == 1);
    used = read_pty_until(
        master, output, used, sizeof output, "Termination cancelled."
    );
    CHECK(kpb_query_status(runtime_dir, "tui-session", &status) == KPB_OK);
    CHECK(write(master, "q", 1) == 1);
    CHECK(waitpid(child, &wait_status, 0) == child);
    CHECK(WIFEXITED(wait_status));
    CHECK(WEXITSTATUS(wait_status) == 0);
    close(master);

    CHECK(kpb_terminate(runtime_dir, "tui-session") == KPB_OK);
    for (wait_status = 0; wait_status < 40; wait_status++) {
        if (kpb_query_status(
                runtime_dir, "tui-session", &status
            ) == KPB_ERR_NOT_FOUND) {
            return;
        }
        usleep(100000);
    }
    CHECK(!"TUI test session disappeared");
}

static int
reader_child(void) {
    static unsigned char buffer[8192];
    const size_t target = 512 * 1024;
    struct termios attributes;
    size_t received = 0;
    if (tcgetattr(STDIN_FILENO, &attributes) != 0) return 2;
    cfmakeraw(&attributes);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) != 0) return 2;
    if (write(STDOUT_FILENO, "READER_READY", 12) != 12) return 2;
    usleep(300000);
    while (received < target) {
        size_t wanted = target - received;
        ssize_t count;
        if (wanted > sizeof buffer) wanted = sizeof buffer;
        count = read(STDIN_FILENO, buffer, wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            return 2;
        }
        if (count == 0) return 2;
        received += (size_t)count;
    }
    dprintf(STDOUT_FILENO, "READER_COUNT=%zu\n", received);
    return received == target ? 0 : 2;
}

static void
test_large_input_backpressure(void) {
    char *command[] = {(char *)test_program, "--reader-child", NULL};
    unsigned char input[KPB_IO_CHUNK];
    unsigned char output[16384];
    kpb_spawn_options options;
    kpb_connection connection;
    size_t used;
    int wait_status;
    int index;

    memset(input, 'x', sizeof input);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "backpressure";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, NULL) == KPB_OK);
    CHECK(kpb_attach(
        runtime_dir, "backpressure", 24, 80, 0, 0, &connection) == KPB_OK);
    used = read_until_replay_done(&connection, output, sizeof output);
    while (!memmem(output, used, "READER_READY", 12)) {
        kpb_event event;
        wait_readable(connection.fd);
        CHECK(kpb_receive(
            &connection, output + used, sizeof output - used, &event) == KPB_OK);
        CHECK(event.type == KPB_EVENT_OUTPUT);
        used += event.size;
    }
    for (index = 0; index < 16; index++) {
        CHECK(kpb_send_input(&connection, input, sizeof input) == KPB_OK);
    }
    wait_status = read_until_exit(&connection, output, &used, sizeof output);
    CHECK(WIFEXITED(wait_status));
    CHECK(WEXITSTATUS(wait_status) == 0);
    CHECK(memmem(output, used, "READER_COUNT=524288", 19) != NULL);
    kpb_detach(&connection);
}

static size_t
read_whole_file(const char *path, unsigned char *buffer, size_t capacity) {
    size_t used = 0;
    FILE *stream = fopen(path, "rb");
    CHECK(stream != NULL);
    while (used < capacity) {
        size_t count = fread(buffer + used, 1, capacity - used, stream);
        if (!count) break;
        used += count;
    }
    fclose(stream);
    return used;
}

/* Wait for a session to run to completion and be reaped.  Transcripts are
 * written by the broker itself, so these tests deliberately never attach a
 * client: that also proves output is captured for an unattached pane. */
static void
wait_for_session_end(const char *session_id) {
    int attempt;
    for (attempt = 0; attempt < 600; attempt++) {
        kpb_status status;
        if (kpb_query_status(runtime_dir, session_id, &status) == KPB_ERR_NOT_FOUND) return;
        usleep(20000);
    }
    CHECK(!"session did not finish");
}

static size_t
run_with_transcript(
    const char *session_id,
    char *const *command,
    const char *transcript_path,
    uint64_t limit,
    int graphics,
    unsigned char *buffer,
    size_t capacity
) {
    kpb_spawn_options options;
    kpb_status status;

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = session_id;
    options.cwd = "/tmp";
    options.argv = command;
    options.transcript_path = transcript_path;
    options.transcript_limit = limit;
    options.transcript_graphics = graphics;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    wait_for_session_end(session_id);
    return read_whole_file(transcript_path, buffer, capacity);
}

static void
test_transcript_elides_graphics(void) {
    /* A 40 KiB payload deliberately exceeds KPB_IO_CHUNK so the APC sequence
     * spans several PTY reads and exercises the resumable scanner. */
    char *command[] = {
        "/bin/sh", "-c",
        "printf 'visible-before\\n'; "
        "printf '\\033_Ga=T,f=100;'; "
        "i=0; while [ $i -lt 640 ]; do "
        "printf 'QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWZnaGlqa2xtbm9wcXJz'; "
        "i=$((i+1)); done; "
        "printf '\\033\\\\'; "
        "printf 'visible-after\\n'",
        NULL
    };
    char transcript[KPB_PATH_MAX];
    unsigned char buffer[262144];
    struct stat status;
    size_t used;

    snprintf(transcript, sizeof transcript, "%s/elide.log", runtime_dir);
    used = run_with_transcript(
        "elide", command, transcript, KPB_DEFAULT_TRANSCRIPT_LIMIT,
        KPB_TRANSCRIPT_GRAPHICS_ELIDE, buffer, sizeof buffer
    );

    CHECK(memmem(buffer, used, "visible-before", 14) != NULL);
    CHECK(memmem(buffer, used, "visible-after", 13) != NULL);
    /* No fragment of the payload, and no APC introducer, survives. */
    CHECK(memmem(buffer, used, "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVph", 36) == NULL);
    CHECK(memmem(buffer, used, "\033_G", 3) == NULL);
    CHECK(memmem(buffer, used, "bytes of graphics elided", 24) != NULL);
    /* The elided transcript is a tiny fraction of the ~40 KiB that was sent. */
    CHECK(used < 4096);

    CHECK(stat(transcript, &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    CHECK(unlink(transcript) == 0);
}

static void
test_transcript_keeps_graphics_when_asked(void) {
    char *command[] = {
        "/bin/sh", "-c", "printf '\\033_Ga=q;PAYLOAD_KEPT\\033\\\\done\\n'", NULL
    };
    char transcript[KPB_PATH_MAX];
    unsigned char buffer[65536];
    size_t used;

    snprintf(transcript, sizeof transcript, "%s/keep.log", runtime_dir);
    used = run_with_transcript(
        "keep", command, transcript, KPB_DEFAULT_TRANSCRIPT_LIMIT,
        KPB_TRANSCRIPT_GRAPHICS_KEEP, buffer, sizeof buffer
    );

    CHECK(memmem(buffer, used, "\033_Ga=q;PAYLOAD_KEPT\033\\", 21) != NULL);
    CHECK(memmem(buffer, used, "done", 4) != NULL);
    CHECK(unlink(transcript) == 0);
}

static void
test_transcript_rotates_and_keeps_newest(void) {
    char *command[] = {
        "/bin/sh", "-c",
        "printf 'OLDEST_LINE\\n'; "
        "i=0; while [ $i -lt 2000 ]; do printf 'filler-%s\\n' \"$i\"; i=$((i+1)); done; "
        "printf 'NEWEST_LINE\\n'",
        NULL
    };
    char transcript[KPB_PATH_MAX];
    unsigned char buffer[262144];
    const uint64_t limit = 16384;
    size_t used;

    snprintf(transcript, sizeof transcript, "%s/rotate.log", runtime_dir);
    used = run_with_transcript(
        "rotate", command, transcript, limit,
        KPB_TRANSCRIPT_GRAPHICS_ELIDE, buffer, sizeof buffer
    );

    /* Bounded, and bounded by the newest bytes rather than the oldest. */
    CHECK(used <= limit);
    CHECK(memmem(buffer, used, "NEWEST_LINE", 11) != NULL);
    CHECK(memmem(buffer, used, "OLDEST_LINE", 11) == NULL);
    CHECK(unlink(transcript) == 0);
}

/* A pane's most useful output is what it printed on its way out.  The child
 * here floods the PTY and exits with no pause, so the bytes are still buffered
 * when it is reaped.
 *
 * This asserts the contract rather than guarding the race: with no client
 * attached the loop drains often enough to pass even without drain_pty().
 * The loss is reproducible through the CLI, where a client is attached and
 * the tail was truncated mid-stream before that drain existed. */
static void
test_transcript_captures_output_written_just_before_exit(void) {
    char *command[] = {
        "/bin/sh", "-c",
        "i=0; while [ $i -lt 400 ]; do "
        "printf 'burst-line-%s-padding-padding-padding-padding\\n' \"$i\"; "
        "i=$((i+1)); done; printf 'FINAL_DYING_WORDS\\n'",
        NULL
    };
    char transcript[KPB_PATH_MAX];
    unsigned char buffer[262144];
    size_t used;

    snprintf(transcript, sizeof transcript, "%s/dying.log", runtime_dir);
    used = run_with_transcript(
        "dying", command, transcript, KPB_DEFAULT_TRANSCRIPT_LIMIT,
        KPB_TRANSCRIPT_GRAPHICS_ELIDE, buffer, sizeof buffer
    );

    CHECK(memmem(buffer, used, "burst-line-0-", 13) != NULL);
    CHECK(memmem(buffer, used, "burst-line-399-", 15) != NULL);
    CHECK(memmem(buffer, used, "FINAL_DYING_WORDS", 17) != NULL);
    CHECK(unlink(transcript) == 0);
}

/* ---- protocol v1 regression fences ------------------------------------
 *
 * These two tests were written and landed against the pre-v2 tree.  They exist
 * to pin the behaviour that protocol v2 must not disturb, so that a later
 * failure points at the v2 change rather than at a rewritten expectation. */

typedef struct {
    unsigned char bytes[65536];
    size_t used;
    int events[16];
    size_t event_count;
    int wait_status;
} capture;

/* A deliberately deterministic child: no dates, no pids, no terminal-size
 * queries.  Input is delivered only once the sentinel that invites it has been
 * seen, so the byte stream cannot depend on scheduling. */
static char *const *
deterministic_command(void) {
    static char *command[] = {
        "/bin/sh", "-c",
        "stty -echo; printf 'S0:'; "
        "IFS= read -r a; printf 'A=%s:' \"$a\"; "
        "IFS= read -r b; printf 'B=%s:DONE\\n' \"$b\"",
        NULL
    };
    return command;
}

static void
capture_record_event(capture *out, int type) {
    CHECK(out->event_count < sizeof out->events / sizeof out->events[0]);
    out->events[out->event_count++] = type;
}

/* Drive one full session through a v1 attach and record every payload byte and
 * the ordered sequence of non-OUTPUT events.  `observers` read-only observers
 * are attached before the read-write client, which is what makes this usable
 * as both halves of the byte-identity guarantee. */
static void
capture_run(const char *session_id, capture *out, size_t observers) {
    kpb_spawn_options options;
    kpb_connection connection;
    kpb_connection watchers[KPB_OBSERVER_MAX];
    kpb_status status;
    size_t index;
    bool sent_a = false;
    bool sent_b = false;

    CHECK(observers <= KPB_OBSERVER_MAX);
    memset(out, 0, sizeof *out);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = session_id;
    options.cwd = "/tmp";
    options.argv = (char *const *)deterministic_command();
    options.rows = 30;
    options.columns = 100;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    for (index = 0; index < observers; index++) {
        kpb_attach_result observed;
        CHECK(kpb_observe(runtime_dir, session_id, &watchers[index], &observed) == KPB_OK);
        CHECK(observed.version == 2);
    }
    CHECK(kpb_attach(runtime_dir, session_id, 30, 100, 0, 0, &connection) == KPB_OK);

    while (true) {
        kpb_event event;
        if (!sent_a && memmem(out->bytes, out->used, "S0:", 3)) {
            CHECK(kpb_send_input(&connection, "alpha\n", 6) == KPB_OK);
            sent_a = true;
        } else if (sent_a && !sent_b && memmem(out->bytes, out->used, "A=alpha:", 8)) {
            CHECK(kpb_send_input(&connection, "omega\n", 6) == KPB_OK);
            sent_b = true;
        }
        wait_readable(connection.fd);
        CHECK(kpb_receive(
            &connection, out->bytes + out->used,
            sizeof out->bytes - out->used, &event) == KPB_OK);
        if (event.type == KPB_EVENT_OUTPUT) {
            out->used += event.size;
            CHECK(out->used < sizeof out->bytes);
            continue;
        }
        capture_record_event(out, (int)event.type);
        if (event.type == KPB_EVENT_EXIT) {
            out->wait_status = event.exit_status;
            break;
        }
        CHECK(event.type == KPB_EVENT_REPLAY_DONE);
    }
    kpb_detach(&connection);
    for (index = 0; index < observers; index++) kpb_detach(&watchers[index]);
    wait_for_session_end(session_id);
}

/* A second read-write attach is refused with the exact v1 error frame, and the
 * refusal touches nothing: the incumbent keeps working afterwards. */
static void
test_busy_refusal_v1(void) {
    static const char expected[] = "session already attached";
    char *command[] = {
        "/bin/sh", "-c",
        "stty -echo; printf 'READY:'; "
        "while IFS= read -r line; do printf 'GOT=%s:' \"$line\"; done",
        NULL
    };
    kpb_spawn_options options;
    kpb_connection first;
    kpb_connection second;
    kpb_event event;
    unsigned char output[16384];
    unsigned char refusal[256];
    size_t used;

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "busy-v1";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, NULL) == KPB_OK);

    CHECK(kpb_attach(runtime_dir, "busy-v1", 24, 80, 0, 0, &first) == KPB_OK);
    used = read_until_replay_done(&first, output, sizeof output);
    while (!memmem(output, used, "READY:", 6)) {
        wait_readable(first.fd);
        CHECK(kpb_receive(&first, output + used, sizeof output - used, &event) == KPB_OK);
        CHECK(event.type == KPB_EVENT_OUTPUT);
        used += event.size;
    }

    /* Attach is fire-and-forget, so the refusal arrives as the first frame. */
    CHECK(kpb_attach(runtime_dir, "busy-v1", 24, 80, 0, 0, &second) == KPB_OK);
    wait_readable(second.fd);
    CHECK(kpb_receive(&second, refusal, sizeof refusal, &event) == KPB_OK);
    CHECK(event.type == KPB_EVENT_ERROR);
    CHECK(event.size == sizeof expected - 1);
    CHECK(memcmp(refusal, expected, sizeof expected - 1) == 0);
    kpb_detach(&second);

    /* Non-vacuity: the incumbent is undisturbed. */
    CHECK(kpb_send_input(&first, "still-here\n", 11) == KPB_OK);
    while (!memmem(output, used, "GOT=still-here:", 15)) {
        wait_readable(first.fd);
        CHECK(kpb_receive(&first, output + used, sizeof output - used, &event) == KPB_OK);
        CHECK(event.type == KPB_EVENT_OUTPUT);
        used += event.size;
    }
    kpb_detach(&first);
    CHECK(kpb_terminate(runtime_dir, "busy-v1") == KPB_OK);
    wait_for_session_end("busy-v1");
}

/* The same scripted session, run twice, must produce the same bytes.  This is
 * the control half of the headline v2 guarantee: once observers exist, the
 * second run attaches them and the assertion below must still hold. */
static void
test_v1_stream_byte_identical(void) {
    static capture run_a;
    static capture run_b;

    /* Run B attaches three observers before the first byte.  If they perturbed
     * the client's stream by even one byte, this fails. */
    capture_run("identical-a", &run_a, 0);
    capture_run("identical-b", &run_b, 3);

    CHECK(run_a.used == run_b.used);
    CHECK(memcmp(run_a.bytes, run_b.bytes, run_a.used) == 0);
    CHECK(run_a.wait_status == run_b.wait_status);
    CHECK(WIFEXITED(run_a.wait_status));
    CHECK(WEXITSTATUS(run_a.wait_status) == 0);
    CHECK(run_a.event_count == run_b.event_count);
    CHECK(memcmp(run_a.events, run_b.events,
                 run_a.event_count * sizeof run_a.events[0]) == 0);
    /* Exactly one replay boundary and one exit, in that order. */
    CHECK(run_a.event_count == 2);
    CHECK(run_a.events[0] == KPB_EVENT_REPLAY_DONE);
    CHECK(run_a.events[1] == KPB_EVENT_EXIT);
    /* Non-vacuity: the capture really contains the scripted conversation. */
    CHECK(memmem(run_a.bytes, run_a.used, "B=omega:DONE", 12) != NULL);
}

/* ---- protocol v2: observers and resume ------------------------------- */

static char *const *
echo_command(void) {
    static char *command[] = {
        "/bin/sh", "-c",
        "stty -echo; printf 'READY:'; "
        "while IFS= read -r line; do printf 'GOT=%s:' \"$line\"; done",
        NULL
    };
    return command;
}

/* Emits a large, quiescent-afterwards stream: enough to overflow an observer
 * queue and to exceed the observer replay bound, then waits for EOF so the
 * session is still alive to be observed. */
#define WRITER_CHILD_BYTES (4U * 1024U * 1024U)

static int
writer_child(void) {
    static unsigned char buffer[8192];
    size_t written = 0;
    memset(buffer, 'x', sizeof buffer);
    while (written < WRITER_CHILD_BYTES) {
        size_t wanted = WRITER_CHILD_BYTES - written;
        ssize_t count;
        if (wanted > sizeof buffer) wanted = sizeof buffer;
        count = write(STDOUT_FILENO, buffer, wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            return 2;
        }
        written += (size_t)count;
    }
    if (write(STDOUT_FILENO, "WRITER_DONE\n", 12) != 12) return 2;
    for (;;) {
        char scratch[256];
        ssize_t count = read(STDIN_FILENO, scratch, sizeof scratch);
        if (count < 0) {
            if (errno == EINTR) continue;
            return 2;
        }
        if (count == 0) return 0;
    }
}

static void
spawn_session(
    const char *session_id,
    char *const *command,
    uint64_t journal_limit
) {
    kpb_spawn_options options;
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = session_id;
    options.cwd = "/tmp";
    options.argv = command;
    options.rows = 30;
    options.columns = 100;
    options.journal_limit = journal_limit;
    CHECK(kpb_spawn(&options, NULL) == KPB_OK);
}

static void
spawn_echo_session(const char *session_id, uint64_t journal_limit) {
    spawn_session(
        session_id, (char *const *)echo_command(),
        journal_limit ? journal_limit : KPB_DEFAULT_JOURNAL_LIMIT);
}

/* Attach read-write at protocol 2, optionally resuming. */
static kpb_result
attach_v2(
    const char *session_id,
    kpb_connection *connection,
    kpb_attach_result *result,
    int resume,
    uint64_t epoch,
    uint64_t offset
) {
    kpb_attach_options options;
    kpb_attach_options_init(&options);
    options.rows = 30;
    options.columns = 100;
    options.resume = resume;
    options.resume_epoch = epoch;
    options.resume_offset = offset;
    return kpb_attach_with_options(
        runtime_dir, session_id, &options, connection, result);
}

static kpb_result
observe_v2(
    const char *session_id,
    kpb_connection *connection,
    kpb_attach_result *result,
    int resume,
    uint64_t epoch,
    uint64_t offset
) {
    kpb_attach_options options;
    kpb_attach_options_init(&options);
    options.mode = KPB_ATTACH_OBSERVE;
    options.resume = resume;
    options.resume_epoch = epoch;
    options.resume_offset = offset;
    return kpb_attach_with_options(
        runtime_dir, session_id, &options, connection, result);
}

/* A detach is processed by the broker on its next poll, so a test that
 * reattaches immediately can legitimately race the departure.  Wait for the
 * read-write slot to be observably free rather than sleeping and hoping. */
static void
wait_until_detached(const char *session_id) {
    kpb_status status;
    int attempt;
    for (attempt = 0; attempt < 200; attempt++) {
        CHECK(kpb_query_status(runtime_dir, session_id, &status) == KPB_OK);
        if (!status.attached) return;
        usleep(10000);
    }
    CHECK(!"read-write slot never became free");
}

/* Journal position while the pane is known to be quiescent. */
static void
journal_position(const char *session_id, uint64_t *epoch, uint64_t *offset) {
    kpb_status status;
    CHECK(kpb_query_status(runtime_dir, session_id, &status) == KPB_OK);
    *epoch = status.journal_epoch;
    *offset = status.journal_bytes;
}

/* Read from one connection until `needle` has been seen. */
static size_t
read_until(kpb_connection *connection, unsigned char *out, size_t used,
           size_t capacity, const char *needle) {
    size_t size = strlen(needle);
    while (!memmem(out, used, needle, size)) {
        kpb_event event;
        wait_readable(connection->fd);
        CHECK(kpb_receive(connection, out + used, capacity - used, &event) == KPB_OK);
        CHECK(event.type == KPB_EVENT_OUTPUT);
        used += event.size;
        CHECK(used < capacity);
    }
    return used;
}

/* Tolerates a session that has already finished on its own: a pane driven to
 * exit can disappear between any check and the request that follows it. */
static void
terminate_and_reap(const char *session_id) {
    kpb_result result = kpb_terminate(runtime_dir, session_id);
    CHECK(result == KPB_OK || result == KPB_ERR_NOT_FOUND);
    wait_for_session_end(session_id);
}

/* A hand-rolled client, so the bytes on the wire can be asserted directly
 * rather than through the library that produces them. */
static int
raw_connect(const char *session_id) {
    struct sockaddr_un address;
    char path[KPB_PATH_MAX];
    int fd;
    CHECK(snprintf(path, sizeof path, "%s/sessions/%s/control.sock",
                   runtime_dir, session_id) < (int)sizeof path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    CHECK(strlen(path) < sizeof address.sun_path);
    memcpy(address.sun_path, path, strlen(path));
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    return fd;
}

static void
raw_write(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, cursor + written, size - written);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        written += (size_t)count;
    }
}

static void
raw_send_frame(int fd, uint16_t type, const void *payload, uint32_t size) {
    kpb_frame_header header;
    header.magic = htonl(KPB_PROTOCOL_MAGIC);
    header.version = htons(KPB_PROTOCOL_VERSION);
    header.type = htons(type);
    header.payload_size = htonl(size);
    raw_write(fd, &header, sizeof header);
    if (size) raw_write(fd, payload, size);
}

static bool
raw_read_exactly(int fd, void *data, size_t size) {
    unsigned char *cursor = data;
    size_t got = 0;
    while (got < size) {
        ssize_t count = read(fd, cursor + got, size - got);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        got += (size_t)count;
    }
    return true;
}

/* A peer that stops mid-frame must not stop the broker.
 *
 * The accept path was bounded first and that was not enough: an attached
 * client's frames are read from the same event loop, so a client that sends one
 * byte of a header and then nothing held the loop indefinitely - no pane
 * output, no status, no accept, and `kill` could not recover it.  Measured on
 * the build that bounded only the accept path, status never returned; with the
 * client read bounded it returns at the deadline.
 *
 * The alarm is deliberate: if this regresses the symptom is a hang, and a test
 * suite that hangs tells you far less than one that dies. */
static void
test_a_stalled_client_does_not_stop_the_broker(void) {
    kpb_wire_winsize size;
    kpb_status status;
    int fd;

    {
        static char *const command[] = {"sleep", "3600", NULL};
        spawn_session("clientstall", command, KPB_DEFAULT_JOURNAL_LIMIT);
    }
    fd = raw_connect("clientstall");
    size.rows = htons(24);
    size.columns = htons(80);
    size.xpixel = 0;
    size.ypixel = 0;
    raw_send_frame(fd, KPB_FRAME_ATTACH, &size, sizeof size);
    {
        /* The attach has to have landed, or the stall would hit the accept
         * path - which is already bounded and is not what this is about. */
        int waited = 0;
        kpb_status probe;
        while (waited++ < 200) {
            if (kpb_query_status(runtime_dir, "clientstall", &probe) == KPB_OK &&
                probe.attached == 1) {
                break;
            }
            usleep(10000);
        }
        CHECK(waited < 200);
    }

    /* One byte of a twelve-byte header, then silence.  The pause matters: the
     * broker has to have noticed the byte and entered the read before the
     * status query arrives, or poll reports both at once, the listener is
     * serviced first, and the test passes without ever reaching the stall. */
    raw_write(fd, "\x4b", 1);
    usleep(300000);

    alarm(20);
    CHECK(kpb_query_status(runtime_dir, "clientstall", &status) == KPB_OK);
    alarm(0);

    close(fd);
    terminate_and_reap("clientstall");
}

/* Every frame a v1 peer receives must carry version 1 and a type it already
 * parses.  This is the test that catches an unconditionally emitted reply. */
static void
test_v1_peer_sees_only_version_1_headers(void) {
    kpb_connection watchers[3];
    kpb_wire_winsize size;
    unsigned char scratch[KPB_IO_CHUNK];
    size_t index;
    bool saw_exit = false;
    bool first = true;
    int fd;

    spawn_echo_session("rawv1", 0);
    for (index = 0; index < 3; index++) {
        CHECK(kpb_observe(runtime_dir, "rawv1", &watchers[index], NULL) == KPB_OK);
    }

    fd = raw_connect("rawv1");
    size.rows = htons(24);
    size.columns = htons(80);
    size.xpixel = 0;
    size.ypixel = 0;
    raw_send_frame(fd, KPB_FRAME_ATTACH, &size, sizeof size);
    /* Drive the pane to completion so this terminates without a timeout: one
     * line to echo, then EOT to close the child's stdin.  Half-closing the
     * socket instead would make the broker drop us as a departed client. */
    raw_send_frame(fd, KPB_FRAME_INPUT, "done\n", 5);
    raw_send_frame(fd, KPB_FRAME_INPUT, "\004", 1);

    while (!saw_exit) {
        kpb_frame_header header;
        uint32_t payload_size;
        uint16_t type;
        CHECK(raw_read_exactly(fd, &header, sizeof header));
        CHECK(ntohl(header.magic) == KPB_PROTOCOL_MAGIC);
        CHECK(ntohs(header.version) == 1);
        type = ntohs(header.type);
        CHECK(type == KPB_FRAME_OUTPUT ||
              type == KPB_FRAME_REPLAY_DONE ||
              type == KPB_FRAME_EXIT);
        if (first) {
            CHECK(type == KPB_FRAME_OUTPUT || type == KPB_FRAME_REPLAY_DONE);
            first = false;
        }
        payload_size = ntohl(header.payload_size);
        while (payload_size) {
            size_t wanted = payload_size < sizeof scratch ? payload_size : sizeof scratch;
            CHECK(raw_read_exactly(fd, scratch, wanted));
            payload_size -= (uint32_t)wanted;
        }
        if (type == KPB_FRAME_EXIT) saw_exit = true;
    }
    close(fd);
    for (index = 0; index < 3; index++) kpb_detach(&watchers[index]);
    /* This pane was driven to exit, so it reaps itself; asking the broker to
     * terminate would race its own shutdown. */
    wait_for_session_end("rawv1");
}

/* An observer takes no read-write slot and does not make the pane look
 * attached, which is what kilix filters reusable panes on. */
static void
test_observe_does_not_claim_slot(void) {
    kpb_connection watchers[2];
    kpb_connection client;
    kpb_attach_result observed;
    kpb_status status;
    unsigned char output[16384];
    size_t index;
    int attempt;

    spawn_echo_session("noslot", 0);
    for (index = 0; index < 2; index++) {
        CHECK(kpb_observe(runtime_dir, "noslot", &watchers[index], &observed) == KPB_OK);
        CHECK(observed.version == 2);
        CHECK(read_until_replay_done(&watchers[index], output, sizeof output) <= sizeof output);
    }
    for (attempt = 0; attempt < 40; attempt++) {
        CHECK(kpb_query_status(runtime_dir, "noslot", &status) == KPB_OK);
        CHECK(status.attached == 0);
        usleep(5000);
    }
    /* A read-write attach still succeeds, and now the pane does look attached. */
    CHECK(kpb_attach(runtime_dir, "noslot", 30, 100, 0, 0, &client) == KPB_OK);
    (void)read_until_replay_done(&client, output, sizeof output);
    for (attempt = 0; attempt < 40; attempt++) {
        CHECK(kpb_query_status(runtime_dir, "noslot", &status) == KPB_OK);
        if (status.attached) break;
        usleep(50000);
    }
    CHECK(status.attached == 1);
    kpb_detach(&client);
    for (index = 0; index < 2; index++) kpb_detach(&watchers[index]);
    terminate_and_reap("noslot");
}

/* Observers and the read-write client see the same bytes in the same order. */
static void
test_observers_receive_identical_bytes(void) {
    kpb_connection watchers[3];
    kpb_connection client;
    kpb_attach_result observed;
    static unsigned char client_bytes[65536];
    static unsigned char watcher_bytes[3][65536];
    size_t client_used;
    size_t watcher_used[3];
    size_t index;

    spawn_echo_session("samebytes", 0);
    CHECK(kpb_attach(runtime_dir, "samebytes", 30, 100, 0, 0, &client) == KPB_OK);
    client_used = read_until_replay_done(&client, client_bytes, sizeof client_bytes);
    client_used = read_until(
        &client, client_bytes, client_used, sizeof client_bytes, "READY:");
    /* Everything before this point is prologue; the comparison window starts
     * once every observer is attached and drained. */
    client_used = 0;

    for (index = 0; index < 3; index++) {
        CHECK(kpb_observe(runtime_dir, "samebytes", &watchers[index], &observed) == KPB_OK);
        CHECK(observed.version == 2);
        (void)read_until_replay_done(
            &watchers[index], watcher_bytes[index], sizeof watcher_bytes[index]);
        watcher_used[index] = 0;
    }

    CHECK(kpb_send_input(&client, "alpha\n", 6) == KPB_OK);
    CHECK(kpb_send_input(&client, "beta\n", 5) == KPB_OK);

    client_used = read_until(
        &client, client_bytes, client_used, sizeof client_bytes, "GOT=beta:");
    for (index = 0; index < 3; index++) {
        watcher_used[index] = read_until(
            &watchers[index], watcher_bytes[index], watcher_used[index],
            sizeof watcher_bytes[index], "GOT=beta:");
        CHECK(watcher_used[index] == client_used);
        CHECK(memcmp(watcher_bytes[index], client_bytes, client_used) == 0);
    }
    CHECK(memmem(client_bytes, client_used, "GOT=alpha:GOT=beta:", 19) != NULL);

    kpb_detach(&client);
    for (index = 0; index < 3; index++) kpb_detach(&watchers[index]);
    terminate_and_reap("samebytes");
}

/* Input from an observer is refused outright, never applied and never
 * silently dropped. */
static void
test_observer_input_refused(void) {
    kpb_connection client;
    kpb_connection watcher;
    kpb_event event;
    static unsigned char output[65536];
    unsigned char refusal[256];
    size_t used;

    spawn_echo_session("readonly", 0);
    CHECK(kpb_attach(runtime_dir, "readonly", 30, 100, 0, 0, &client) == KPB_OK);
    used = read_until_replay_done(&client, output, sizeof output);
    used = read_until(&client, output, used, sizeof output, "READY:");
    CHECK(kpb_observe(runtime_dir, "readonly", &watcher, NULL) == KPB_OK);
    (void)read_until_replay_done(&watcher, refusal, sizeof refusal);

    /* Hand-built so the library cannot refuse it on our behalf. */
    raw_send_frame(watcher.fd, KPB_FRAME_INPUT, "OBS_INJECT\n", 11);
    wait_readable(watcher.fd);
    CHECK(kpb_receive(&watcher, refusal, sizeof refusal, &event) == KPB_OK);
    CHECK(event.type == KPB_EVENT_ERROR);
    CHECK(event.size == strlen(KPB_ERROR_READ_ONLY));
    CHECK(memcmp(refusal, KPB_ERROR_READ_ONLY, event.size) == 0);

    /* Non-vacuity: the pane is alive and still accepts real input, and the
     * injected sentinel never appears. */
    CHECK(kpb_send_input(&client, "RW_OK\n", 6) == KPB_OK);
    used = read_until(&client, output, used, sizeof output, "GOT=RW_OK:");
    CHECK(memmem(output, used, "OBS_INJECT", 10) == NULL);

    close(watcher.fd);
    watcher.fd = -1;
    kpb_detach(&client);
    terminate_and_reap("readonly");
}

/* An observer can neither resize at admission nor by sending a resize. */
static void
test_observer_resize_refused(void) {
    kpb_connection client;
    kpb_connection watcher;
    kpb_wire_attach request;
    kpb_wire_winsize wire;
    kpb_event event;
    kpb_status status;
    static unsigned char output[65536];
    unsigned char refusal[256];
    size_t used;
    int attempt;
    int fd;

    spawn_echo_session("noresize", 0);
    CHECK(kpb_attach(runtime_dir, "noresize", 30, 100, 0, 0, &client) == KPB_OK);
    used = read_until_replay_done(&client, output, sizeof output);
    used = read_until(&client, output, used, sizeof output, "READY:");
    CHECK(kpb_query_status(runtime_dir, "noresize", &status) == KPB_OK);
    CHECK(status.rows == 30 && status.columns == 100);

    /* Admission carrying non-zero dimensions must not reach apply_size: the
     * v1 path calls it unconditionally, so this is the direct check. */
    fd = raw_connect("noresize");
    memset(&request, 0, sizeof request);
    request.rows = htons(9);
    request.columns = htons(9);
    request.version = htons(2);
    request.mode = htons(KPB_WIRE_MODE_OBSERVE);
    raw_send_frame(fd, KPB_FRAME_OBSERVE, &request, sizeof request);
    for (attempt = 0; attempt < 20; attempt++) {
        CHECK(kpb_query_status(runtime_dir, "noresize", &status) == KPB_OK);
        CHECK(status.rows == 30 && status.columns == 100);
        usleep(5000);
    }
    close(fd);

    CHECK(kpb_observe(runtime_dir, "noresize", &watcher, NULL) == KPB_OK);
    (void)read_until_replay_done(&watcher, refusal, sizeof refusal);
    wire.rows = htons(9);
    wire.columns = htons(9);
    wire.xpixel = 0;
    wire.ypixel = 0;
    raw_send_frame(watcher.fd, KPB_FRAME_RESIZE, &wire, sizeof wire);
    wait_readable(watcher.fd);
    CHECK(kpb_receive(&watcher, refusal, sizeof refusal, &event) == KPB_OK);
    CHECK(event.type == KPB_EVENT_ERROR);
    CHECK(event.size == strlen(KPB_ERROR_READ_ONLY));
    CHECK(kpb_query_status(runtime_dir, "noresize", &status) == KPB_OK);
    CHECK(status.rows == 30 && status.columns == 100);

    /* Non-vacuity: a resize from the read-write client IS honoured. */
    CHECK(kpb_resize(&client, 40, 120, 0, 0) == KPB_OK);
    for (attempt = 0; attempt < 40; attempt++) {
        CHECK(kpb_query_status(runtime_dir, "noresize", &status) == KPB_OK);
        if (status.rows == 40 && status.columns == 120) break;
        usleep(50000);
    }
    CHECK(status.rows == 40 && status.columns == 120);

    close(watcher.fd);
    watcher.fd = -1;
    kpb_detach(&client);
    terminate_and_reap("noresize");
}

/* Backpressure from one observer must not reach the PTY loop or the
 * read-write client.  This is the highest-risk failure mode of the change:
 * the read-write client's send is deliberately blocking, and an observer
 * sharing that property would freeze the pane for everyone. */
static void
test_stalled_observer_does_not_wedge_pane(void) {
    char *command[] = {(char *)test_program, "--writer-child", NULL};
    kpb_connection client;
    kpb_connection watchers[KPB_OBSERVER_MAX];
    unsigned char buffer[KPB_IO_CHUNK];
    size_t x_count = 0;
    size_t index;
    int wait_status = -1;
    int attempts;
    bool closed = false;

    spawn_session("stalled", command, KPB_DEFAULT_JOURNAL_LIMIT);
    CHECK(kpb_attach(runtime_dir, "stalled", 30, 100, 0, 0, &client) == KPB_OK);
    /* A full complement, every one of them attached and then never read
     * again.  If any of them could apply back-pressure, the pane stops. */
    for (index = 0; index < KPB_OBSERVER_MAX; index++) {
        CHECK(kpb_observe(runtime_dir, "stalled", &watchers[index], NULL) == KPB_OK);
    }

    while (true) {
        kpb_event event;
        size_t index;
        wait_readable(client.fd);
        CHECK(kpb_receive(&client, buffer, sizeof buffer, &event) == KPB_OK);
        if (event.type == KPB_EVENT_OUTPUT) {
            for (index = 0; index < event.size; index++) {
                if (buffer[index] == 'x') x_count++;
            }
            if (x_count == WRITER_CHILD_BYTES && !closed) {
                /* End the pane: EOT closes the child's stdin. */
                CHECK(kpb_send_input(&client, "\004", 1) == KPB_OK);
                closed = true;
            }
            continue;
        }
        if (event.type == KPB_EVENT_REPLAY_DONE) continue;
        CHECK(event.type == KPB_EVENT_EXIT);
        wait_status = event.exit_status;
        break;
    }
    /* Every byte arrived, within wait_readable's per-poll bound, and the
     * child's own exit status proves it wrote its trailer too. */
    CHECK(x_count == WRITER_CHILD_BYTES);
    CHECK(WIFEXITED(wait_status));
    CHECK(WEXITSTATUS(wait_status) == 0);

    /* Each stalled observer was dropped rather than allowed to block anything:
     * drain whatever it had buffered and expect the connection to end. */
    for (index = 0; index < KPB_OBSERVER_MAX; index++) {
        for (attempts = 0; attempts < 8192; attempts++) {
            ssize_t count = recv(
                watchers[index].fd, buffer, sizeof buffer, MSG_DONTWAIT);
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd descriptor = {
                        .fd = watchers[index].fd, .events = POLLIN, .revents = 0};
                    CHECK(poll(&descriptor, 1, 3000) > 0);
                    continue;
                }
                break;
            }
        }
        CHECK(attempts < 8192);
        close(watchers[index].fd);
        watchers[index].fd = -1;
    }
    kpb_detach(&client);
    wait_for_session_end("stalled");
}

static void
test_observer_capacity(void) {
    kpb_connection watchers[KPB_OBSERVER_MAX];
    kpb_connection overflow;
    kpb_connection client;
    kpb_attach_result observed;
    kpb_status status;
    static unsigned char output[65536];
    unsigned char scratch[16384];
    size_t index;
    size_t used;

    spawn_echo_session("capacity", 0);
    CHECK(kpb_attach(runtime_dir, "capacity", 30, 100, 0, 0, &client) == KPB_OK);
    used = read_until_replay_done(&client, output, sizeof output);
    used = read_until(&client, output, used, sizeof output, "READY:");

    for (index = 0; index < KPB_OBSERVER_MAX; index++) {
        CHECK(kpb_observe(runtime_dir, "capacity", &watchers[index], &observed) == KPB_OK);
        CHECK(observed.version == 2);
        (void)read_until_replay_done(&watchers[index], scratch, sizeof scratch);
    }
    /* The refusal is a decoded result code, not a string match. */
    memset(&overflow, 0, sizeof overflow);
    CHECK(kpb_observe(runtime_dir, "capacity", &overflow, NULL) == KPB_ERR_BUSY);
    CHECK(overflow.fd == -1);

    /* Neither the accepted set nor the pane was disturbed. */
    CHECK(kpb_send_input(&client, "after\n", 6) == KPB_OK);
    used = read_until(&client, output, used, sizeof output, "GOT=after:");
    for (index = 0; index < KPB_OBSERVER_MAX; index++) {
        size_t seen = read_until(
            &watchers[index], scratch, 0, sizeof scratch, "GOT=after:");
        CHECK(seen > 0);
    }
    CHECK(kpb_query_status(runtime_dir, "capacity", &status) == KPB_OK);
    CHECK(status.attached == 1);

    for (index = 0; index < KPB_OBSERVER_MAX; index++) kpb_detach(&watchers[index]);
    kpb_detach(&client);
    terminate_and_reap("capacity");
}

#ifdef __linux__
static size_t
count_open_descriptors(pid_t pid) {
    char path[64];
    struct dirent *entry;
    DIR *directory;
    size_t count = 0;
    CHECK(snprintf(path, sizeof path, "/proc/%ld/fd", (long)pid) < (int)sizeof path);
    directory = opendir(path);
    CHECK(directory != NULL);
    while ((entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        count++;
    }
    closedir(directory);
    return count;
}
#endif

/* Churn must leak neither slots nor descriptors.  Explicit, because the broker
 * leaves through _exit and so is never seen by LeakSanitizer. */
static void
test_observer_slots_and_fds_reclaimed(void) {
    kpb_connection watchers[KPB_OBSERVER_MAX];
    kpb_connection replacement;
    kpb_status status;
    unsigned char scratch[16384];
    size_t index;
    int cycle;

    spawn_echo_session("reclaim", 0);
    CHECK(kpb_query_status(runtime_dir, "reclaim", &status) == KPB_OK);

    for (index = 0; index < KPB_OBSERVER_MAX; index++) {
        CHECK(kpb_observe(runtime_dir, "reclaim", &watchers[index], NULL) == KPB_OK);
        (void)read_until_replay_done(&watchers[index], scratch, sizeof scratch);
    }
    /* A departing observer frees its slot for a newcomer. */
    kpb_detach(&watchers[3]);
    watchers[3].fd = -1;
    for (cycle = 0; cycle < 40; cycle++) {
        if (kpb_observe(runtime_dir, "reclaim", &replacement, NULL) == KPB_OK) break;
        usleep(25000);
    }
    CHECK(cycle < 40);
    (void)read_until_replay_done(&replacement, scratch, sizeof scratch);
    kpb_detach(&replacement);
    for (index = 0; index < KPB_OBSERVER_MAX; index++) {
        if (watchers[index].fd >= 0) kpb_detach(&watchers[index]);
    }

#ifdef __linux__
    {
        size_t before;
        size_t after;
        /* Let the broker notice the departures before counting. */
        usleep(200000);
        before = count_open_descriptors(status.broker_pid);
        for (cycle = 0; cycle < 64; cycle++) {
            kpb_connection watcher;
            CHECK(kpb_observe(runtime_dir, "reclaim", &watcher, NULL) == KPB_OK);
            (void)read_until_replay_done(&watcher, scratch, sizeof scratch);
            /* Alternate the polite path with a crash: a frontend that dies
             * never announces itself, and that is the harder case. */
            if (cycle % 2) {
                kpb_detach(&watcher);
            } else {
                close(watcher.fd);
            }
            usleep(2000);
        }
        usleep(300000);
        after = count_open_descriptors(status.broker_pid);
        CHECK(after == before);
    }
#endif
    terminate_and_reap("reclaim");
}

/* An observer that vanishes without announcing itself is reaped cleanly, and
 * the read-write client does not notice. */
static void
test_observer_hard_disconnect_does_not_disturb_client(void) {
    kpb_connection client;
    kpb_connection watchers[3];
    static unsigned char output[65536];
    unsigned char scratch[16384];
    size_t index;
    size_t used;

    spawn_echo_session("harddrop", 0);
    CHECK(kpb_attach(runtime_dir, "harddrop", 30, 100, 0, 0, &client) == KPB_OK);
    used = read_until_replay_done(&client, output, sizeof output);
    used = read_until(&client, output, used, sizeof output, "READY:");
    for (index = 0; index < 3; index++) {
        CHECK(kpb_observe(runtime_dir, "harddrop", &watchers[index], NULL) == KPB_OK);
        (void)read_until_replay_done(&watchers[index], scratch, sizeof scratch);
    }
    CHECK(kpb_send_input(&client, "before\n", 7) == KPB_OK);
    used = read_until(&client, output, used, sizeof output, "GOT=before:");

    /* Deliberately not kpb_detach: no DETACH frame, just a vanished peer. */
    for (index = 0; index < 3; index++) {
        close(watchers[index].fd);
        watchers[index].fd = -1;
    }
    CHECK(kpb_send_input(&client, "after\n", 6) == KPB_OK);
    used = read_until(&client, output, used, sizeof output, "GOT=after:");
    CHECK(memmem(output, used, "GOT=before:GOT=after:", 21) != NULL);

    CHECK(kpb_send_input(&client, "\004", 1) == KPB_OK);
    {
        int wait_status = read_until_exit(&client, output, &used, sizeof output);
        CHECK(WIFEXITED(wait_status));
        CHECK(WEXITSTATUS(wait_status) == 0);
    }
    kpb_detach(&client);
    wait_for_session_end("harddrop");
}

/* Observers are terminal-state citizens: they see EXIT, and one that refuses
 * to read cannot hold teardown open. */
static void
test_observers_see_exit_and_teardown_is_clean(void) {
    kpb_connection client;
    kpb_connection watchers[3];
    kpb_connection deaf;
    static unsigned char output[65536];
    unsigned char scratch[16384];
    size_t index;
    size_t used;
    int client_status;

    spawn_echo_session("exitfan", 0);
    CHECK(kpb_attach(runtime_dir, "exitfan", 30, 100, 0, 0, &client) == KPB_OK);
    used = read_until_replay_done(&client, output, sizeof output);
    used = read_until(&client, output, used, sizeof output, "READY:");
    for (index = 0; index < 3; index++) {
        CHECK(kpb_observe(runtime_dir, "exitfan", &watchers[index], NULL) == KPB_OK);
        (void)read_until_replay_done(&watchers[index], scratch, sizeof scratch);
    }
    /* One observer never reads again. */
    CHECK(kpb_observe(runtime_dir, "exitfan", &deaf, NULL) == KPB_OK);

    CHECK(kpb_send_input(&client, "\004", 1) == KPB_OK);
    client_status = read_until_exit(&client, output, &used, sizeof output);
    CHECK(WIFEXITED(client_status));
    for (index = 0; index < 3; index++) {
        size_t seen = 0;
        int observed_status = read_until_exit(
            &watchers[index], scratch, &seen, sizeof scratch);
        CHECK(observed_status == client_status);
        kpb_detach(&watchers[index]);
    }
    close(deaf.fd);
    deaf.fd = -1;
    kpb_detach(&client);
    wait_for_session_end("exitfan");
}

/* Read exactly one attach reply from a hand-rolled connection. */
static void
raw_read_attach_reply(int fd, kpb_wire_attach_reply *reply) {
    kpb_frame_header header;
    CHECK(raw_read_exactly(fd, &header, sizeof header));
    CHECK(ntohl(header.magic) == KPB_PROTOCOL_MAGIC);
    CHECK(ntohs(header.version) == KPB_PROTOCOL_VERSION);
    CHECK(ntohs(header.type) == KPB_FRAME_ATTACH_REPLY);
    CHECK(ntohl(header.payload_size) == sizeof *reply);
    CHECK(raw_read_exactly(fd, reply, sizeof *reply));
}

static void
test_version_negotiation(void) {
    kpb_connection connection;
    kpb_attach_result result;
    kpb_wire_attach request;
    kpb_wire_attach_reply reply;
    kpb_wire_winsize size;
    kpb_attach_options options;
    kpb_status status;
    kpb_event event;
    unsigned char scratch[16384];
    int fd;

    /* The frame-format version is the thing that must never move: a status
     * query round-trips only when both sides agree on it exactly. */
    CHECK(kpb_protocol_version() == 1);
    CHECK(kpb_protocol_version_max() == 2);

    spawn_echo_session("negotiate", 0);
    CHECK(kpb_query_status(runtime_dir, "negotiate", &status) == KPB_OK);

    CHECK(attach_v2("negotiate", &connection, &result, 0, 0, 0) == KPB_OK);
    CHECK(result.version == 2);
    (void)read_until_replay_done(&connection, scratch, sizeof scratch);
    kpb_detach(&connection);
    wait_until_detached("negotiate");

    /* A client that offers more than the server has is clamped, not refused. */
    kpb_attach_options_init(&options);
    options.rows = 30;
    options.columns = 100;
    options.max_version = 99;
    CHECK(kpb_attach_with_options(
        runtime_dir, "negotiate", &options, &connection, &result) == KPB_OK);
    CHECK(result.version == 2);
    (void)read_until_replay_done(&connection, scratch, sizeof scratch);
    kpb_detach(&connection);
    wait_until_detached("negotiate");

    /* max_version below 2 emits an ordinary v1 attach: no reply frame, which
     * read_until_replay_done proves by never seeing an unparseable type. */
    kpb_attach_options_init(&options);
    options.rows = 30;
    options.columns = 100;
    options.max_version = 1;
    CHECK(kpb_attach_with_options(
        runtime_dir, "negotiate", &options, &connection, &result) == KPB_OK);
    CHECK(result.version == 1);
    (void)read_until_replay_done(&connection, scratch, sizeof scratch);
    kpb_detach(&connection);
    wait_until_detached("negotiate");

    /* Observe and resume are honest failures below version 2, never a silent
     * downgrade to something weaker than what was asked for. */
    kpb_attach_options_init(&options);
    options.max_version = 1;
    options.mode = KPB_ATTACH_OBSERVE;
    CHECK(kpb_attach_with_options(
        runtime_dir, "negotiate", &options, &connection, NULL) == KPB_ERR_INVALID);
    kpb_attach_options_init(&options);
    options.max_version = 1;
    options.resume = 1;
    CHECK(kpb_attach_with_options(
        runtime_dir, "negotiate", &options, &connection, NULL) == KPB_ERR_INVALID);

    /* A 32-byte request declaring it cannot speak version 2 contradicts
     * itself; the refusal still reports the true ceiling. */
    fd = raw_connect("negotiate");
    memset(&request, 0, sizeof request);
    request.version = htons(1);
    request.mode = htons(KPB_WIRE_MODE_CONTROL);
    raw_send_frame(fd, KPB_FRAME_ATTACH, &request, sizeof request);
    raw_read_attach_reply(fd, &reply);
    CHECK(ntohs(reply.result) == KPB_ERR_PROTOCOL);
    CHECK(ntohs(reply.version) == 2);
    close(fd);

    /* Mode must agree with the frame type. */
    fd = raw_connect("negotiate");
    memset(&request, 0, sizeof request);
    request.version = htons(2);
    request.mode = htons(KPB_WIRE_MODE_OBSERVE);
    raw_send_frame(fd, KPB_FRAME_ATTACH, &request, sizeof request);
    raw_read_attach_reply(fd, &reply);
    CHECK(ntohs(reply.result) == KPB_ERR_PROTOCOL);
    close(fd);

    /* An observe request in v1 clothing is told what it needs. */
    CHECK(kpb_attach(runtime_dir, "negotiate", 30, 100, 0, 0, &connection) == KPB_OK);
    (void)read_until_replay_done(&connection, scratch, sizeof scratch);
    {
        kpb_connection probe;
        memset(&probe, 0, sizeof probe);
        size.rows = htons(24);
        size.columns = htons(80);
        size.xpixel = 0;
        size.ypixel = 0;
        probe.fd = raw_connect("negotiate");
        raw_send_frame(probe.fd, KPB_FRAME_OBSERVE, &size, sizeof size);
        wait_readable(probe.fd);
        CHECK(kpb_receive(&probe, scratch, sizeof scratch, &event) == KPB_OK);
        CHECK(event.type == KPB_EVENT_ERROR);
        CHECK(event.size == strlen(KPB_ERROR_NEEDS_V2));
        CHECK(memcmp(scratch, KPB_ERROR_NEEDS_V2, event.size) == 0);
        close(probe.fd);
    }
    CHECK(kpb_query_status(runtime_dir, "negotiate", &status) == KPB_OK);
    kpb_detach(&connection);
    terminate_and_reap("negotiate");
}

/* Resuming at a retained offset replays exactly what arrived after it: no
 * gap, no duplicate. */
static void
test_resume_streams_forward(void) {
    kpb_connection first;
    kpb_connection second;
    kpb_connection third;
    kpb_connection watcher;
    kpb_attach_result result;
    static unsigned char output[65536];
    uint64_t epoch0;
    uint64_t offset0;
    uint64_t epoch1;
    uint64_t offset1;
    size_t used;

    spawn_echo_session("resume", 0);
    CHECK(attach_v2("resume", &first, &result, 0, 0, 0) == KPB_OK);
    CHECK(result.version == 2);
    CHECK(result.resumed == 0);
    used = read_until_replay_done(&first, output, sizeof output);
    used = read_until(&first, output, used, sizeof output, "READY:");
    CHECK(kpb_send_input(&first, "AAA\n", 4) == KPB_OK);
    used = read_until(&first, output, used, sizeof output, "GOT=AAA:");
    /* The child is now blocked on read, so the journal is quiescent. */
    journal_position("resume", &epoch0, &offset0);
    kpb_detach(&first);
    wait_until_detached("resume");

    CHECK(attach_v2("resume", &second, &result, 0, 0, 0) == KPB_OK);
    CHECK(result.resumed == 0);
    used = read_until_replay_done(&second, output, sizeof output);
    CHECK(memmem(output, used, "GOT=AAA:", 8) != NULL);
    CHECK(kpb_send_input(&second, "BBB\n", 4) == KPB_OK);
    used = read_until(&second, output, used, sizeof output, "GOT=BBB:");
    journal_position("resume", &epoch1, &offset1);
    kpb_detach(&second);
    wait_until_detached("resume");
    CHECK(epoch1 == epoch0);
    CHECK(offset1 > offset0);

    CHECK(attach_v2("resume", &third, &result, 1, epoch0, offset0) == KPB_OK);
    CHECK(result.resumed == 1);
    CHECK(result.journal_epoch == epoch0);
    CHECK(result.journal_offset == offset0);
    used = read_until_replay_done(&third, output, sizeof output);
    CHECK(used == (size_t)(offset1 - offset0));
    CHECK(memmem(output, used, "GOT=BBB:", 8) != NULL);
    CHECK(memmem(output, used, "GOT=AAA:", 8) == NULL);
    /* Live output still follows the resumed history. */
    CHECK(kpb_send_input(&third, "CCC\n", 4) == KPB_OK);
    used = read_until(&third, output, used, sizeof output, "GOT=CCC:");
    kpb_detach(&third);
    wait_until_detached("resume");

    /* Resuming exactly at the end yields nothing but stays live. */
    journal_position("resume", &epoch1, &offset1);
    CHECK(observe_v2("resume", &watcher, &result, 1, epoch1, offset1) == KPB_OK);
    CHECK(result.resumed == 1);
    CHECK(result.journal_offset == offset1);
    CHECK(read_until_replay_done(&watcher, output, sizeof output) == 0);
    CHECK(attach_v2("resume", &first, &result, 0, 0, 0) == KPB_OK);
    (void)read_until_replay_done(&first, output, sizeof output);
    CHECK(kpb_send_input(&first, "DDD\n", 4) == KPB_OK);
    (void)read_until(&watcher, output, 0, sizeof output, "GOT=DDD:");
    kpb_detach(&watcher);
    kpb_detach(&first);
    terminate_and_reap("resume");
}

/* A rolled-over epoch is the only way an offset is evicted, and it is not
 * honoured. */
static void
test_resume_stale_epoch_falls_back(void) {
    char *command[] = {
        "/bin/sh", "-c",
        "stty -echo; printf 'READY:'; "
        "while IFS= read -r line; do "
        "i=0; while [ $i -lt 120 ]; do "
        "printf '0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF01'; "
        "i=$((i+1)); done; printf ':GOT=%s:' \"$line\"; done",
        NULL
    };
    kpb_connection connection;
    kpb_attach_result result;
    kpb_status status;
    static unsigned char output[65536];
    uint64_t epoch0;
    uint64_t offset0;
    size_t used;

    /* A small journal makes the rollover deterministic without megabytes. */
    spawn_session("staleepoch", command, 4096);
    CHECK(attach_v2("staleepoch", &connection, &result, 0, 0, 0) == KPB_OK);
    used = read_until_replay_done(&connection, output, sizeof output);
    used = read_until(&connection, output, used, sizeof output, "READY:");
    journal_position("staleepoch", &epoch0, &offset0);

    CHECK(kpb_send_input(&connection, "flood\n", 6) == KPB_OK);
    used = read_until(&connection, output, used, sizeof output, "GOT=flood:");
    CHECK(kpb_query_status(runtime_dir, "staleepoch", &status) == KPB_OK);
    CHECK(status.journal_epoch > epoch0);
    CHECK(status.replay_complete == 0);
    kpb_detach(&connection);
    wait_until_detached("staleepoch");

    CHECK(attach_v2("staleepoch", &connection, &result, 1, epoch0, offset0) == KPB_OK);
    CHECK(result.resumed == 0);
    CHECK(result.journal_offset == 0);
    CHECK(result.replay_complete == 0);
    used = read_until_replay_done(&connection, output, sizeof output);
    CHECK(used == (size_t)status.journal_bytes);
    CHECK(used >= 2);
    CHECK(memcmp(output, "\033c", 2) == 0);
    kpb_detach(&connection);
    terminate_and_reap("staleepoch");
}

/* An offset the journal does not hold is rejected safely. */
static void
test_resume_offset_past_end_falls_back(void) {
    kpb_connection connection;
    kpb_connection watcher;
    kpb_attach_result result;
    static unsigned char output[65536];
    uint64_t epoch;
    uint64_t offset;
    size_t used;

    spawn_echo_session("pastend", 0);
    CHECK(attach_v2("pastend", &connection, &result, 0, 0, 0) == KPB_OK);
    used = read_until_replay_done(&connection, output, sizeof output);
    used = read_until(&connection, output, used, sizeof output, "READY:");
    journal_position("pastend", &epoch, &offset);
    kpb_detach(&connection);
    wait_until_detached("pastend");

    CHECK(attach_v2("pastend", &connection, &result, 1, epoch, offset + 4096) == KPB_OK);
    CHECK(result.resumed == 0);
    CHECK(result.journal_offset == 0);
    used = read_until_replay_done(&connection, output, sizeof output);
    CHECK(used == (size_t)offset);
    CHECK(memmem(output, used, "READY:", 6) != NULL);
    CHECK(kpb_send_input(&connection, "live\n", 5) == KPB_OK);
    used = read_until(&connection, output, used, sizeof output, "GOT=live:");

    CHECK(observe_v2("pastend", &watcher, &result, 1, epoch, offset + 4096) == KPB_OK);
    CHECK(result.resumed == 0);
    CHECK(result.journal_offset == 0);
    (void)read_until_replay_done(&watcher, output, sizeof output);
    kpb_detach(&watcher);
    kpb_detach(&connection);
    terminate_and_reap("pastend");
}

/* An observer owed more history than the replay bound gets a trimmed,
 * explicitly flagged stream rather than being silently dropped. */
static void
test_observer_replay_truncation_is_flagged(void) {
    char *command[] = {(char *)test_program, "--writer-child", NULL};
    kpb_connection client;
    kpb_connection watcher;
    kpb_attach_result result;
    unsigned char buffer[KPB_IO_CHUNK];
    kpb_status status;
    size_t x_count = 0;
    size_t replayed = 0;
    bool first_payload = true;

    /* An unbounded journal, so the only trimming is the observer's. */
    spawn_session("truncate", command, 0);
    CHECK(kpb_attach(runtime_dir, "truncate", 30, 100, 0, 0, &client) == KPB_OK);
    while (x_count < WRITER_CHILD_BYTES) {
        kpb_event event;
        size_t index;
        wait_readable(client.fd);
        CHECK(kpb_receive(&client, buffer, sizeof buffer, &event) == KPB_OK);
        if (event.type != KPB_EVENT_OUTPUT) continue;
        for (index = 0; index < event.size; index++) {
            if (buffer[index] == 'x') x_count++;
        }
    }
    /* The child is now blocked on read, so the journal has stopped growing. */
    CHECK(kpb_query_status(runtime_dir, "truncate", &status) == KPB_OK);
    CHECK(status.journal_bytes > KPB_OBSERVER_REPLAY_MAX);

    CHECK(kpb_observe(runtime_dir, "truncate", &watcher, &result) == KPB_OK);
    CHECK(result.truncated == 1);
    CHECK(result.resumed == 0);
    CHECK(result.journal_offset == status.journal_bytes - KPB_OBSERVER_REPLAY_MAX);
    while (true) {
        kpb_event event;
        wait_readable(watcher.fd);
        CHECK(kpb_receive(&watcher, buffer, sizeof buffer, &event) == KPB_OK);
        if (event.type == KPB_EVENT_REPLAY_DONE) break;
        if (first_payload) {
            /* The trimmed stream is self-correcting: it opens with a reset.
             * That reset arrives as its own event, not as output, because it
             * is not journal content - see the drift check below. */
            CHECK(event.type == KPB_EVENT_RESET);
            CHECK(event.size == 2);
            CHECK(memcmp(buffer, "\033c", 2) == 0);
            first_payload = false;
            continue;
        }
        CHECK(event.type == KPB_EVENT_OUTPUT);
        replayed += event.size;
    }
    /* The reset is not in this total, and that is the point: a consumer that
     * starts at result.journal_offset and adds every OUTPUT byte lands exactly
     * on the journal's end.  While the reset was sent as OUTPUT it landed two
     * bytes past it, and a later resume from that offset skipped two bytes of
     * real output. */
    CHECK(replayed == KPB_OBSERVER_REPLAY_MAX);
    CHECK(result.journal_offset + replayed == status.journal_bytes);

    /* Not dropped: it is still attached and still receives terminal state. */
    CHECK(kpb_send_input(&client, "\004", 1) == KPB_OK);
    {
        size_t seen = 0;
        int observed = read_until_exit(&watcher, buffer, &seen, sizeof buffer);
        CHECK(WIFEXITED(observed));
        CHECK(WEXITSTATUS(observed) == 0);
    }
    kpb_detach(&watcher);
    kpb_detach(&client);
    wait_for_session_end("truncate");
}

static void
test_transcript_absent_by_default(void) {
    char *command[] = {"/bin/sh", "-c", "printf 'no-transcript\\n'", NULL};
    kpb_spawn_options options;
    kpb_status status;

    kpb_spawn_options_init(&options);
    CHECK(options.transcript_path == NULL);
    CHECK(options.transcript_limit == KPB_DEFAULT_TRANSCRIPT_LIMIT);
    CHECK(options.transcript_graphics == KPB_TRANSCRIPT_GRAPHICS_ELIDE);
    options.runtime_dir = runtime_dir;
    options.session_id = "notranscript";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    wait_for_session_end("notranscript");
}

int
main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--reader-child") == 0) {
        return reader_child();
    }
    if (argc == 2 && strcmp(argv[1], "--writer-child") == 0) {
        return writer_child();
    }
    CHECK(realpath(argv[0], test_program_path) != NULL);
    test_program = test_program_path;
    CHECK(mkdtemp(runtime_dir) != NULL);
    CHECK(chmod(runtime_dir, 0700) == 0);
    RUN(test_ids);
    CHECK(kpb_prepare_runtime(runtime_dir) == KPB_OK);
    RUN(test_spawn_detach_replay_and_exit);
    RUN(test_transcript_elides_graphics);
    RUN(test_transcript_keeps_graphics_when_asked);
    RUN(test_transcript_rotates_and_keeps_newest);
    RUN(test_transcript_captures_output_written_just_before_exit);
    RUN(test_transcript_absent_by_default);
    RUN(test_large_input_backpressure);
    RUN(test_busy_refusal_v1);
    RUN(test_v1_stream_byte_identical);
    RUN(test_v1_peer_sees_only_version_1_headers);
    RUN(test_observe_does_not_claim_slot);
    RUN(test_observers_receive_identical_bytes);
    RUN(test_observer_input_refused);
    RUN(test_observer_resize_refused);
    RUN(test_stalled_observer_does_not_wedge_pane);
    RUN(test_observer_capacity);
    RUN(test_observer_slots_and_fds_reclaimed);
    RUN(test_observer_hard_disconnect_does_not_disturb_client);
    RUN(test_observers_see_exit_and_teardown_is_clean);
    RUN(test_version_negotiation);
    RUN(test_resume_streams_forward);
    RUN(test_resume_stale_epoch_falls_back);
    RUN(test_resume_offset_past_end_falls_back);
    RUN(test_observer_replay_truncation_is_flagged);
    RUN(test_tui);
    RUN(test_a_stalled_client_does_not_stop_the_broker);
    RUN(test_terminate);
    {
        char sessions[4096];
        snprintf(sessions, sizeof sessions, "%s/sessions", runtime_dir);
        CHECK(rmdir(sessions) == 0);
        CHECK(rmdir(runtime_dir) == 0);
    }
    puts("all kitty-pty-broker tests passed");
    return 0;
}
