#define _GNU_SOURCE

#include "kitty_pty_broker.h"

#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s (errno=%d %s)\n", \
                __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        exit(1); \
    } \
} while (0)

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
        master, output, used, sizeof output, "Kilix PTY Sessions"
    );
    used = read_pty_until(
        master, output, used, sizeof output, "tui-session"
    );
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
 * the ordered sequence of non-OUTPUT events. */
static void
capture_run(const char *session_id, capture *out) {
    kpb_spawn_options options;
    kpb_connection connection;
    kpb_status status;
    bool sent_a = false;
    bool sent_b = false;

    memset(out, 0, sizeof *out);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = session_id;
    options.cwd = "/tmp";
    options.argv = (char *const *)deterministic_command();
    options.rows = 30;
    options.columns = 100;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
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

    capture_run("identical-a", &run_a);
    capture_run("identical-b", &run_b);

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
    CHECK(realpath(argv[0], test_program_path) != NULL);
    test_program = test_program_path;
    CHECK(mkdtemp(runtime_dir) != NULL);
    CHECK(chmod(runtime_dir, 0700) == 0);
    test_ids();
    CHECK(kpb_prepare_runtime(runtime_dir) == KPB_OK);
    test_spawn_detach_replay_and_exit();
    test_transcript_elides_graphics();
    test_transcript_keeps_graphics_when_asked();
    test_transcript_rotates_and_keeps_newest();
    test_transcript_captures_output_written_just_before_exit();
    test_transcript_absent_by_default();
    test_large_input_backpressure();
    test_busy_refusal_v1();
    test_v1_stream_byte_identical();
    test_tui();
    test_terminate();
    {
        char sessions[4096];
        snprintf(sessions, sizeof sessions, "%s/sessions", runtime_dir);
        CHECK(rmdir(sessions) == 0);
        CHECK(rmdir(runtime_dir) == 0);
    }
    puts("all kitty-pty-broker tests passed");
    return 0;
}
