#define _GNU_SOURCE

#include "kitty_pty_broker.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ftw.h>
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
#include <time.h>
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

#define FAIL(message) do { \
    fprintf(stderr, "%s:%d: [%s] %s (errno=%d %s)\n", \
            __FILE__, __LINE__, current_test, message, \
            errno, strerror(errno)); \
    exit(1); \
} while (0)

/* `test-broker --only NAME` runs the one named test, which is how a mutation
 * check shows that exactly the test written for a fix is the one that fails. */
static const char *only_test;
static int tests_run;

/* Name the running test so a timeout inside a shared helper is attributable. */
#define RUN(test) do { \
    if (only_test && strcmp(only_test, #test) != 0) break; \
    current_test = #test; \
    tests_run++; \
    test(); \
} while (0)

static char runtime_dir[] = "/tmp/kitty-pty-broker-test.XXXXXX";
static char test_program_path[KPB_PATH_MAX];
static const char *test_program;

static void wait_for_session_end(const char *session_id);
static int count_session(const kpb_status *status, void *data);
static void raw_write(int fd, const void *data, size_t size);

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
    FAIL("terminated session did not disappear");
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
    (void)read_pty_until(
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
    FAIL("TUI test session did not disappear");
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

static int
descriptor_child(const char *text) {
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || *end || value < 0 || value > INT32_MAX) return 2;
    if (write((int)value, "D", 1) != 1) return 2;
    if (close((int)value) != 0) return 2;
    sleep(30);
    return 0;
}

static void
test_inherited_descriptors_are_command_only(void) {
    int descriptors[2];
    char descriptor_text[32];
    char *command[] = {
        (char *)test_program, "--descriptor-child", descriptor_text, NULL
    };
    kpb_spawn_options options;
    unsigned char marker = 0;
    bool saw_eof = false;
    int flags;
    int attempt;

    CHECK(pipe(descriptors) == 0);
    CHECK(snprintf(
        descriptor_text, sizeof descriptor_text, "%d", descriptors[1]) > 0);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "descriptor-scope";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, NULL) == KPB_OK);
    CHECK(close(descriptors[1]) == 0);
    wait_readable(descriptors[0]);
    CHECK(read(descriptors[0], &marker, 1) == 1);
    CHECK(marker == 'D');

    flags = fcntl(descriptors[0], F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK(fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0);
    for (attempt = 0; attempt < 100; attempt++) {
        ssize_t count = read(descriptors[0], &marker, 1);
        if (count == 0) {
            saw_eof = true;
            break;
        }
        CHECK(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        usleep(10000);
    }
    CHECK(kpb_terminate(runtime_dir, "descriptor-scope") == KPB_OK);
    wait_for_session_end("descriptor-scope");
    close(descriptors[0]);
    CHECK(saw_eof);
}

static void
test_child_signal_mask_and_wait_status(void) {
    char *command[] = {
        "/bin/sh", "-c", "stty -echo; IFS= read -r _; kill -TERM $$; exit 99", NULL
    };
    kpb_spawn_options options;
    kpb_connection connection;
    unsigned char output[256];
    sigset_t blocked;
    sigset_t previous;
    struct sigaction ignored;
    struct sigaction previous_child;
    size_t used = 0;
    int wait_status;

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
    memset(&ignored, 0, sizeof ignored);
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    CHECK(sigaction(SIGCHLD, &ignored, &previous_child) == 0);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "signal-mask";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, NULL) == KPB_OK);
    CHECK(sigaction(SIGCHLD, &previous_child, NULL) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
    CHECK(kpb_attach(
        runtime_dir, "signal-mask", 24, 80, 0, 0, &connection) == KPB_OK);
    used = read_until_replay_done(&connection, output, sizeof output);
    CHECK(kpb_send_input(&connection, "go\n", 3) == KPB_OK);
    wait_status = read_until_exit(
        &connection, output, &used, sizeof output);
    CHECK(WIFSIGNALED(wait_status));
    CHECK(WTERMSIG(wait_status) == SIGTERM);
    kpb_detach(&connection);
    wait_for_session_end("signal-mask");
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
    FAIL("session did not finish");
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

static size_t
count_occurrences(
    const unsigned char *data,
    size_t size,
    const char *needle
) {
    size_t count = 0;
    size_t needle_size = strlen(needle);
    while (size >= needle_size) {
        const unsigned char *found = memmem(data, size, needle, needle_size);
        size_t consumed;
        if (!found) break;
        count++;
        consumed = (size_t)(found - data) + needle_size;
        data += consumed;
        size -= consumed;
    }
    return count;
}

static void
test_transcript_scanner_boundaries_and_dense_markers(void) {
    char *command[] = {
        "/bin/sh", "-c",
        "printf 'plain:\\033X:'; "
        "printf '\\033_Qnongraphics\\033\\\\:'; "
        "printf '\\033_Gabc\\033Xdef\\033\\\\after-st:'; "
        "printf '\\033_Gbell\\007after-bel:'; "
        "i=0; while [ $i -lt 1000 ]; do printf '\\033_G\\007'; i=$((i+1)); done; "
        "printf 'tail'",
        NULL
    };
    char transcript[KPB_PATH_MAX];
    static unsigned char buffer[262144];
    size_t used;

    snprintf(transcript, sizeof transcript, "%s/scanner.log", runtime_dir);
    used = run_with_transcript(
        "scanner", command, transcript, KPB_DEFAULT_TRANSCRIPT_LIMIT,
        KPB_TRANSCRIPT_GRAPHICS_ELIDE, buffer, sizeof buffer
    );
    CHECK(memmem(buffer, used, "plain:\033X:", sizeof "plain:\033X:" - 1) != NULL);
    CHECK(memmem(
        buffer, used,
        "\033_Qnongraphics\033\\:", sizeof "\033_Qnongraphics\033\\:" - 1) != NULL);
    CHECK(memmem(buffer, used, "after-st:", 9) != NULL);
    CHECK(memmem(buffer, used, "after-bel:", 10) != NULL);
    CHECK(memmem(buffer, used, "tail", 4) != NULL);
    CHECK(memmem(buffer, used, "abc", 3) == NULL);
    CHECK(memmem(buffer, used, "def", 3) == NULL);
    CHECK(memmem(buffer, used, "bell", 4) == NULL);
    CHECK(count_occurrences(
        buffer, used, "bytes of graphics elided") == 1002);
    CHECK(unlink(transcript) == 0);
}

static void
test_existing_transcript_is_private_and_bounded_immediately(void) {
    char *command[] = {"/bin/sh", "-c", "sleep 1", NULL};
    char transcript[KPB_PATH_MAX];
    unsigned char block[16384];
    kpb_spawn_options options;
    kpb_status status;
    struct stat file_status;
    int fd;
    int attempt;

    memset(block, 'x', sizeof block);
    snprintf(transcript, sizeof transcript, "%s/existing.log", runtime_dir);
    fd = open(transcript, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0);
    raw_write(fd, block, sizeof block);
    CHECK(close(fd) == 0);
    CHECK(chmod(transcript, 0644) == 0);

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "existing-transcript";
    options.cwd = "/tmp";
    options.argv = command;
    options.transcript_path = transcript;
    options.transcript_limit = 4096;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    CHECK(stat(transcript, &file_status) == 0);
    CHECK((file_status.st_mode & 0777) == 0600);
    CHECK(file_status.st_size <= 4096);
    for (attempt = 0; attempt < 40; attempt++) {
        if (kpb_query_status(
                runtime_dir, "existing-transcript", &status
            ) == KPB_ERR_NOT_FOUND) {
            break;
        }
        usleep(50000);
    }
    CHECK(attempt < 40);
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
    /* The opening size record was dropped with the oldest output, so the
     * rotated file starts with a fresh one that also says it was cut. */
    {
        static const char rotated[] = "\033_kilix-transcript;rows=24;cols=80;rotated=1\033\\";
        CHECK(used > sizeof rotated - 1U);
        CHECK(memcmp(buffer, rotated, sizeof rotated - 1U) == 0);
        CHECK(count_occurrences(buffer, used, rotated) == 1);
    }
    CHECK(unlink(transcript) == 0);
}

static void
test_transcript_records_pane_size_and_resizes(void) {
    char *command[] = {"/bin/sh", "-c", "read line; printf 'after-%s\\n' \"$line\"", NULL};
    static const char opened[] = "\033_kilix-transcript;rows=30;cols=100\033\\";
    static const char attached[] = "\033_kilix-transcript;rows=40;cols=120\033\\";
    static const char resized[] = "\033_kilix-transcript;rows=50;cols=132\033\\";
    char transcript[KPB_PATH_MAX];
    unsigned char buffer[65536];
    kpb_spawn_options options;
    kpb_connection connection;
    kpb_status status;
    unsigned char *first, *second, *third, *output;
    size_t used;

    snprintf(transcript, sizeof transcript, "%s/size.log", runtime_dir);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "size";
    options.cwd = "/tmp";
    options.argv = command;
    options.rows = 30;
    options.columns = 100;
    options.transcript_path = transcript;
    options.transcript_limit = KPB_DEFAULT_TRANSCRIPT_LIMIT;
    options.transcript_graphics = KPB_TRANSCRIPT_GRAPHICS_ELIDE;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    CHECK(kpb_attach(runtime_dir, "size", 40, 120, 0, 0, &connection) == KPB_OK);
    CHECK(kpb_resize(&connection, 50, 132, 0, 0) == KPB_OK);
    /* Re-sending the current size is not a change and records nothing. */
    CHECK(kpb_resize(&connection, 50, 132, 0, 0) == KPB_OK);
    CHECK(kpb_send_input(&connection, "go\n", 3) == KPB_OK);
    {
        unsigned char received[KPB_IO_CHUNK];
        bool exited = false;
        while (!exited) {
            kpb_event event;
            wait_readable(connection.fd);
            CHECK(kpb_receive(&connection, received, sizeof received, &event) == KPB_OK);
            CHECK(event.type != KPB_EVENT_ERROR);
            exited = event.type == KPB_EVENT_EXIT;
        }
    }
    kpb_detach(&connection);
    wait_for_session_end("size");
    used = read_whole_file(transcript, buffer, sizeof buffer);

    first = memmem(buffer, used, opened, sizeof opened - 1U);
    second = memmem(buffer, used, attached, sizeof attached - 1U);
    third = memmem(buffer, used, resized, sizeof resized - 1U);
    output = memmem(buffer, used, "after-go", 8);
    CHECK(first == buffer);
    CHECK(second != NULL && second > first);
    CHECK(third != NULL && third > second);
    CHECK(output != NULL && output > third);
    CHECK(count_occurrences(buffer, used, resized) == 1);
    CHECK(memmem(buffer, used, ";rotated=1", 10) == NULL);
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

/* writer_child, gated on one byte of input first.  A test that needs the
 * flood to start only after its client is attached cannot use the plain
 * writer: the journal would otherwise absorb the whole flood before any
 * client-facing backpressure could engage. */
static int
gated_writer_child(void) {
    struct termios attributes;
    unsigned char trigger;
    if (tcgetattr(STDIN_FILENO, &attributes) != 0) return 2;
    cfmakeraw(&attributes);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) != 0) return 2;
    for (;;) {
        ssize_t count = read(STDIN_FILENO, &trigger, 1);
        if (count == 1) break;
        if (count < 0 && errno == EINTR) continue;
        return 2;
    }
    return writer_child();
}

#define FINISHING_WRITER_BYTES (8U * 1024U * 1024U)

/* Fill the journal, then hold the pane open until one byte of input arrives,
 * then exit with a status the test can recognise.  Raw mode, so neither the
 * trigger byte's echo nor output post-processing can change the byte count
 * the journal ends up holding. */
static int
finishing_writer_child(void) {
    static unsigned char buffer[8192];
    struct termios attributes;
    unsigned char trigger;
    size_t written = 0;
    if (tcgetattr(STDIN_FILENO, &attributes) != 0) return 2;
    cfmakeraw(&attributes);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) != 0) return 2;
    memset(buffer, 'x', sizeof buffer);
    while (written < FINISHING_WRITER_BYTES) {
        size_t wanted = FINISHING_WRITER_BYTES - written;
        ssize_t count;
        if (wanted > sizeof buffer) wanted = sizeof buffer;
        count = write(STDOUT_FILENO, buffer, wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            return 2;
        }
        written += (size_t)count;
    }
    for (;;) {
        ssize_t count = read(STDIN_FILENO, &trigger, 1);
        if (count == 1) return 42;
        if (count < 0 && errno == EINTR) continue;
        return 2;
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
    FAIL("read-write slot never became free");
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

/* A client that attaches and then never READS must not stop the broker
 * either.  This was the write-side half of the same hole: sends to the
 * read-write client were blocking, so a frontend that stopped reading -
 * Ctrl-S flow control, SIGSTOP, or simply hung - wedged the loop inside
 * sendmsg once the socket filled.  No status, no accept, and `kill` hung
 * forever, even though kill is the documented deliberate termination path.
 *
 * The backpressure itself is deliberate and must survive the fix: what stops
 * is the shell, held by the kernel PTY buffer once the broker stops reading
 * it, never the broker.  So this asserts both halves - the pane's output
 * genuinely freezes short of everything the child wants to write, and the
 * control plane keeps answering.
 *
 * The alarm mirrors the read-side test above: if this regresses, the symptom
 * is a hang, and a suite that hangs says far less than one that dies. */
static void
test_a_write_stalled_client_does_not_stop_the_broker(void) {
    char *command[] = {(char *)test_program, "--gated-writer-child", NULL};
    kpb_connection connection;
    kpb_status status;
    uint64_t stalled_bytes = 0;
    int attempt;

    alarm(60);
    spawn_session("writestall", command, KPB_DEFAULT_JOURNAL_LIMIT);
    CHECK(kpb_attach(runtime_dir, "writestall", 30, 100, 0, 0, &connection) == KPB_OK);
    /* Release the flood, then never read a single byte of it. */
    CHECK(kpb_send_input(&connection, "x", 1) == KPB_OK);

    /* The journal must freeze: once the client's backlog passes the broker's
     * high-water mark the PTY stops being read and the writer blocks.  Two
     * equal non-zero readings half a second apart call it frozen. */
    for (attempt = 0; attempt < 120; attempt++) {
        CHECK(kpb_query_status(runtime_dir, "writestall", &status) == KPB_OK);
        if (status.journal_bytes && status.journal_bytes == stalled_bytes) break;
        stalled_bytes = status.journal_bytes;
        usleep(500000);
    }
    CHECK(attempt < 120);
    /* Frozen strictly short of the flood: the writer was stopped by
     * backpressure, not drained to completion and not dropped. */
    CHECK(stalled_bytes > 0);
    CHECK(stalled_bytes < WRITER_CHILD_BYTES);
    CHECK(status.attached == 1);

    /* And the deliberate termination path still works. */
    CHECK(kpb_terminate(runtime_dir, "writestall") == KPB_OK);
    wait_for_session_end("writestall");
    alarm(0);

    close(connection.fd);
    connection.fd = -1;
}

/* The exit drain must not cost a live reader its replay tail or the EXIT
 * frame.  A client that attaches just before the child exits can still be
 * owed most of a large journal, and delivering that takes however long the
 * client takes to read it - the drain budget exists to cut off a client that
 * has stopped reading, not one that reads slowly.  So: attach against a full
 * journal, trigger the exit immediately, and read the whole replay at a pace
 * chosen to overrun any fixed drain budget many times over while never
 * pausing long enough to look stalled.  Every byte, REPLAY_DONE, and the
 * child's real exit status must still arrive. */
static void
test_slow_reader_keeps_replay_tail_and_exit(void) {
    char *command[] = {(char *)test_program, "--finishing-writer-child", NULL};
    kpb_connection connection;
    kpb_attach_result result;
    kpb_status status;
    unsigned char buffer[KPB_IO_CHUNK];
    uint64_t received = 0;
    bool replay_done = false;
    int attempt;

    alarm(120);
    spawn_session("slowfinish", command, KPB_DEFAULT_JOURNAL_LIMIT);
    for (attempt = 0; attempt < 600; attempt++) {
        CHECK(kpb_query_status(runtime_dir, "slowfinish", &status) == KPB_OK);
        if (status.journal_bytes >= FINISHING_WRITER_BYTES) break;
        usleep(50000);
    }
    CHECK(attempt < 600);
    CHECK(status.journal_bytes == FINISHING_WRITER_BYTES);

    CHECK(attach_v2("slowfinish", &connection, &result, 0, 0, 0) == KPB_OK);
    CHECK(result.journal_offset == 0);
    CHECK(kpb_send_input(&connection, "x", 1) == KPB_OK);

    while (true) {
        kpb_event event;
        wait_readable(connection.fd);
        CHECK(kpb_receive(&connection, buffer, sizeof buffer, &event) == KPB_OK);
        if (event.type == KPB_EVENT_EXIT) {
            CHECK(replay_done);
            CHECK(WIFEXITED(event.exit_status));
            CHECK(WEXITSTATUS(event.exit_status) == 42);
            break;
        }
        if (event.type == KPB_EVENT_REPLAY_DONE) {
            replay_done = true;
            continue;
        }
        CHECK(event.type == KPB_EVENT_OUTPUT);
        received += event.size;
        /* Slow, never stalled: each pause is far inside the broker's idle
         * allowance, while the drain as a whole takes whole seconds. */
        usleep(5000);
    }
    CHECK(received == FINISHING_WRITER_BYTES);
    kpb_detach(&connection);
    wait_for_session_end("slowfinish");
    alarm(0);
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
    (void)read_until(
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
    (void)read_until(&client, output, used, sizeof output, "READY:");
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
    (void)read_until(&client, output, used, sizeof output, "READY:");
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
    /* A full complement, every one of them attached and then never read
     * again.  Admit them before the read-write client: that client's
     * backpressure is allowed to stop the pane itself, and a pane stalled on
     * its account would confound the property this test isolates. */
    for (index = 0; index < KPB_OBSERVER_MAX; index++) {
        CHECK(kpb_observe(runtime_dir, "stalled", &watchers[index], NULL) == KPB_OK);
    }
    CHECK(kpb_attach(runtime_dir, "stalled", 30, 100, 0, 0, &client) == KPB_OK);

    while (true) {
        kpb_event event;
        size_t byte_index;
        wait_readable(client.fd);
        CHECK(kpb_receive(&client, buffer, sizeof buffer, &event) == KPB_OK);
        if (event.type == KPB_EVENT_OUTPUT) {
            for (byte_index = 0; byte_index < event.size; byte_index++) {
                if (buffer[byte_index] == 'x') x_count++;
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
    (void)read_until(&client, output, used, sizeof output, "GOT=after:");
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

    /* Values beyond the library's own ceiling still mean "at least the
     * current version" rather than wrapping or advertising a future protocol
     * this client does not actually implement. */
    kpb_attach_options_init(&options);
    options.rows = 30;
    options.columns = 100;
    options.max_version = 65536;
    CHECK(kpb_attach_with_options(
        runtime_dir, "negotiate", &options, &connection, &result) == KPB_OK);
    CHECK(result.version == 2);
    (void)read_until_replay_done(&connection, scratch, sizeof scratch);
    kpb_detach(&connection);
    wait_until_detached("negotiate");

    connection.fd = 123;
    memset(&result, 0xff, sizeof result);
    kpb_attach_options_init(&options);
    options.max_version = 0;
    CHECK(kpb_attach_with_options(
        runtime_dir, "negotiate", &options, &connection, &result) == KPB_ERR_INVALID);
    CHECK(connection.fd == -1);
    CHECK(result.version == 0);

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

    /* Unknown request flags are rejected rather than silently changing the
     * meaning of a future client request. */
    fd = raw_connect("negotiate");
    memset(&request, 0, sizeof request);
    request.version = htons(2);
    request.mode = htons(KPB_WIRE_MODE_CONTROL);
    request.flags = htonl(0x80000000U);
    raw_send_frame(fd, KPB_FRAME_ATTACH, &request, sizeof request);
    raw_read_attach_reply(fd, &reply);
    CHECK(ntohs(reply.result) == KPB_ERR_PROTOCOL);
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
    (void)read_until(&first, output, used, sizeof output, "GOT=AAA:");
    /* The child is now blocked on read, so the journal is quiescent. */
    journal_position("resume", &epoch0, &offset0);
    kpb_detach(&first);
    wait_until_detached("resume");

    CHECK(attach_v2("resume", &second, &result, 0, 0, 0) == KPB_OK);
    CHECK(result.resumed == 0);
    used = read_until_replay_done(&second, output, sizeof output);
    CHECK(memmem(output, used, "GOT=AAA:", 8) != NULL);
    CHECK(kpb_send_input(&second, "BBB\n", 4) == KPB_OK);
    (void)read_until(&second, output, used, sizeof output, "GOT=BBB:");
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
    (void)read_until(&third, output, used, sizeof output, "GOT=CCC:");
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
    (void)read_until(&connection, output, used, sizeof output, "GOT=flood:");
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
    (void)read_until(&connection, output, used, sizeof output, "READY:");
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
    (void)read_until(&connection, output, used, sizeof output, "GOT=live:");

    CHECK(observe_v2("pastend", &watcher, &result, 1, epoch, offset + 4096) == KPB_OK);
    CHECK(result.resumed == 0);
    CHECK(result.journal_offset == 0);
    (void)read_until_replay_done(&watcher, output, sizeof output);
    kpb_detach(&watcher);
    kpb_detach(&connection);
    terminate_and_reap("pastend");
}

/* A frame larger than the caller's buffer is skipped to preserve framing, and
 * the skip is reported rather than silent: KPB_ERR_BUFFER carries the size
 * and would-be type of what was lost, and the next frame still parses. */
static void
test_receive_reports_a_skipped_oversized_frame(void) {
    kpb_connection connection;
    kpb_event event;
    unsigned char big[8192];
    unsigned char small[512];
    int sockets[2];

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    memset(big, 'y', sizeof big);
    raw_send_frame(sockets[0], KPB_FRAME_OUTPUT, big, sizeof big);
    raw_send_frame(sockets[0], KPB_FRAME_OUTPUT, "tail", 4);
    memset(&connection, 0, sizeof connection);
    connection.fd = sockets[1];

    memset(&event, 0xa5, sizeof event);
    CHECK(kpb_receive(&connection, small, sizeof small, &event) == KPB_ERR_BUFFER);
    CHECK(event.type == KPB_EVENT_OUTPUT);
    CHECK(event.size == sizeof big);
    /* Framing survives the skip: the next frame is delivered intact. */
    CHECK(kpb_receive(&connection, small, sizeof small, &event) == KPB_OK);
    CHECK(event.type == KPB_EVENT_OUTPUT);
    CHECK(event.size == 4);
    CHECK(memcmp(small, "tail", 4) == 0);
    close(sockets[0]);
    close(sockets[1]);
}

/* The reported skip is actionable: skipped output is journal content, so a
 * caller that kept its cursor can resume from it and get the bytes back. */
static void
test_skipped_output_is_recoverable_by_resume(void) {
    kpb_connection connection;
    kpb_attach_result result;
    kpb_event event;
    unsigned char tiny[600];
    static unsigned char output[65536];
    static char line[2048];
    kpb_status status;
    uint64_t skipped = 0;
    uint64_t received = 0;
    size_t used;
    bool replay_done = false;

    spawn_echo_session("skiprecover", 0);
    CHECK(attach_v2("skiprecover", &connection, &result, 0, 0, 0) == KPB_OK);
    used = read_until_replay_done(&connection, output, sizeof output);
    used = read_until(&connection, output, used, sizeof output, "READY:");
    /* One echo far larger than the small buffer below. */
    memset(line, 'A', 1600);
    line[1600] = '\n';
    CHECK(kpb_send_input(&connection, line, 1601) == KPB_OK);
    /* The echo ends "...AAAA:", and the broker journals bytes before it
     * forwards them, so once the trailing colon arrives here the journal is
     * complete and, with the child blocked on read again, quiescent. */
    (void)read_until(&connection, output, used, sizeof output, "AAAA:");
    kpb_detach(&connection);
    wait_until_detached("skiprecover");
    /* The pane is idle again; the whole journal replays as one frame. */
    CHECK(kpb_query_status(runtime_dir, "skiprecover", &status) == KPB_OK);
    CHECK(status.journal_bytes > sizeof tiny);
    CHECK(status.journal_bytes <= KPB_IO_CHUNK);

    CHECK(attach_v2("skiprecover", &connection, &result, 0, 0, 0) == KPB_OK);
    CHECK(result.journal_offset == 0);
    while (!replay_done) {
        kpb_result received_result;
        wait_readable(connection.fd);
        received_result = kpb_receive(&connection, tiny, sizeof tiny, &event);
        if (received_result == KPB_ERR_BUFFER) {
            CHECK(event.type == KPB_EVENT_OUTPUT);
            CHECK(event.size > sizeof tiny);
            skipped += event.size;
            continue;
        }
        CHECK(received_result == KPB_OK);
        if (event.type == KPB_EVENT_REPLAY_DONE) replay_done = true;
        else {
            CHECK(event.type == KPB_EVENT_OUTPUT);
            received += event.size;
        }
    }
    CHECK(skipped > 0);
    CHECK(skipped + received == status.journal_bytes);
    kpb_detach(&connection);
    wait_until_detached("skiprecover");

    /* Resume from the pre-skip cursor with an adequate buffer replays every
     * byte the skip destroyed. */
    CHECK(attach_v2(
        "skiprecover", &connection, &result,
        1, status.journal_epoch, 0) == KPB_OK);
    CHECK(result.resumed == 1);
    used = read_until_replay_done(&connection, output, sizeof output);
    CHECK(used == (size_t)status.journal_bytes);
    CHECK(memmem(output, used, "AAAAAAAA", 8) != NULL);
    kpb_detach(&connection);
    terminate_and_reap("skiprecover");
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

static int
ignore_session(const kpb_status *status, void *data) {
    (void)status;
    (void)data;
    return 0;
}

/* A broker killed outright - SIGKILL, the OOM killer - cannot run its own
 * cleanup, so its session directory used to be permanent: invisible to list,
 * blocking its ID against respawn, and unremovable by anything in the module.
 * The metadata file names the broker pid precisely so a later walk can prove
 * the process is gone and reap the corpse. */
static void
test_dead_session_directory_is_reaped(void) {
    char *command[] = {"/bin/sh", "-c", "sleep 3600", NULL};
    char session_dir[KPB_PATH_MAX];
    kpb_spawn_options options;
    kpb_status status;
    struct stat probe;

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "corpse";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    CHECK(snprintf(
        session_dir, sizeof session_dir, "%s/sessions/corpse",
        runtime_dir) < (int)sizeof session_dir);

    /* Kill the broker the way the OOM killer would: no cleanup runs.  This
     * process is the broker's parent, so also reap the zombie - a pid that
     * still exists, even as a zombie, must and does count as alive. */
    CHECK(kill(status.broker_pid, SIGKILL) == 0);
    CHECK(waitpid(status.broker_pid, NULL, 0) == status.broker_pid);
    (void)kill(status.child_pid, SIGKILL);
    CHECK(lstat(session_dir, &probe) == 0);

    /* Respawning under the same ID reaps the corpse instead of EXISTS. */
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    terminate_and_reap("corpse");

    /* And a corpse a spawn never touches is reaped by the list walk. */
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    CHECK(kill(status.broker_pid, SIGKILL) == 0);
    CHECK(waitpid(status.broker_pid, NULL, 0) == status.broker_pid);
    (void)kill(status.child_pid, SIGKILL);
    CHECK(lstat(session_dir, &probe) == 0);
    CHECK(kpb_list(runtime_dir, ignore_session, NULL) == KPB_OK);
    CHECK(lstat(session_dir, &probe) != 0 && errno == ENOENT);
}

/* Reaping must never take a live session, so everything short of proof is
 * left alone: a running session, a directory whose recorded broker pid still
 * exists even though nothing answers its socket, and a directory with no
 * metadata at all (a spawn may be mid-flight) all survive a list walk and
 * all keep blocking their IDs. */
static void
test_reaping_never_removes_a_live_session(void) {
    char *command[] = {"/bin/sh", "-c", "sleep 3600", NULL};
    char live_dir[KPB_PATH_MAX];
    char hollow_dir[KPB_PATH_MAX];
    char hollow_metadata[KPB_PATH_MAX];
    char bare_dir[KPB_PATH_MAX];
    kpb_spawn_options options;
    kpb_status status;
    struct stat probe;
    FILE *stream;

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "alive";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, NULL) == KPB_OK);
    CHECK(snprintf(
        live_dir, sizeof live_dir, "%s/sessions/alive",
        runtime_dir) < (int)sizeof live_dir);

    /* Looks dead - no socket answers - but the recorded broker pid is this
     * very process, which is as provably alive as it gets. */
    CHECK(snprintf(
        hollow_dir, sizeof hollow_dir, "%s/sessions/hollow",
        runtime_dir) < (int)sizeof hollow_dir);
    CHECK(snprintf(
        hollow_metadata, sizeof hollow_metadata, "%s/metadata",
        hollow_dir) < (int)sizeof hollow_metadata);
    CHECK(mkdir(hollow_dir, 0700) == 0);
    stream = fopen(hollow_metadata, "w");
    CHECK(stream != NULL);
    CHECK(fprintf(
        stream,
        "version=1\nid=hollow\nbroker_pid=%ld\nchild_pid=1\nstarted_millis=0\n",
        (long)getpid()) > 0);
    CHECK(fclose(stream) == 0);

    CHECK(snprintf(
        bare_dir, sizeof bare_dir, "%s/sessions/bare",
        runtime_dir) < (int)sizeof bare_dir);
    CHECK(mkdir(bare_dir, 0700) == 0);

    CHECK(kpb_list(runtime_dir, ignore_session, NULL) == KPB_OK);

    CHECK(lstat(live_dir, &probe) == 0);
    CHECK(lstat(hollow_dir, &probe) == 0);
    CHECK(lstat(bare_dir, &probe) == 0);
    CHECK(kpb_query_status(runtime_dir, "alive", &status) == KPB_OK);

    /* Neither impostor's ID has become spawnable. */
    options.session_id = "hollow";
    CHECK(kpb_spawn(&options, NULL) == KPB_ERR_EXISTS);
    options.session_id = "bare";
    CHECK(kpb_spawn(&options, NULL) == KPB_ERR_EXISTS);

    CHECK(unlink(hollow_metadata) == 0);
    CHECK(rmdir(hollow_dir) == 0);
    CHECK(rmdir(bare_dir) == 0);
    terminate_and_reap("alive");
}


/* --- client deadlines, runtimes, reaping (RC6) ---------------------------- */

static long
now_millis(void) {
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int
remove_tree_entry(const char *path, const struct stat *status, int flag, struct FTW *info) {
    (void)status;
    (void)flag;
    (void)info;
    return remove(path);
}

static void
remove_tree(const char *path) {
    (void)nftw(path, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* A private scratch runtime for tests that must not share the common one. */
static void
make_scratch_runtime(char *buffer, size_t capacity) {
    static const char pattern[] = "/tmp/kpbt.XXXXXX";
    CHECK(capacity > sizeof pattern);
    memcpy(buffer, pattern, sizeof pattern);
    CHECK(mkdtemp(buffer) != NULL);
}

typedef struct {
    int healthy;
    int unreachable;
    int timed_out;
    char reachable_ids[8][KPB_SESSION_ID_MAX + 1];
    char unreachable_ids[8][KPB_SESSION_ID_MAX + 1];
} list_tally;

static int
tally_entry(const kpb_list_entry *entry, void *data) {
    list_tally *tally = data;
    if (entry->reachable) {
        CHECK(entry->error == KPB_OK);
        CHECK(strcmp(entry->session_id, entry->status.session_id) == 0);
        if (tally->healthy < 8) {
            strcpy(tally->reachable_ids[tally->healthy], entry->session_id);
        }
        tally->healthy++;
    } else {
        if (tally->unreachable < 8) {
            strcpy(tally->unreachable_ids[tally->unreachable], entry->session_id);
        }
        tally->unreachable++;
        if (entry->error == KPB_ERR_TIMEOUT) tally->timed_out++;
    }
    return 0;
}

static bool
tally_has(char ids[8][KPB_SESSION_ID_MAX + 1], int count, const char *id) {
    int index;
    for (index = 0; index < count && index < 8; index++) {
        if (strcmp(ids[index], id) == 0) return true;
    }
    return false;
}

static pid_t
spawn_sleeper(const char *session_id) {
    char *command[] = {"/bin/sh", "-c", "sleep 3600", NULL};
    kpb_spawn_options options;
    kpb_status status;
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = session_id;
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    return status.broker_pid;
}

static void
resume_and_end(pid_t broker, const char *session_id) {
    (void)kill(broker, SIGCONT);
    terminate_and_reap(session_id);
}

/* The finding: SIGSTOP one broker and `list`, `status` and `kill` hung, and the
 * healthy session was never printed.  Each call is now bounded, and a stopped
 * broker neither hangs the caller nor hides a healthy one. */
static void
test_a_stopped_broker_does_not_hang_clients(void) {
    kpb_status status;
    kpb_connection connection;
    list_tally tally;
    kpb_list_options options;
    pid_t broker;
    long started;
    long elapsed;

    broker = spawn_sleeper("wedged");
    (void)spawn_sleeper("healthy");
    CHECK(kill(broker, SIGSTOP) == 0);

    started = now_millis();
    CHECK(kpb_query_status_timeout(runtime_dir, "wedged", &status, 300) == KPB_ERR_TIMEOUT);
    elapsed = now_millis() - started;
    CHECK(elapsed >= 250 && elapsed < 1500);

    started = now_millis();
    CHECK(kpb_terminate_timeout(runtime_dir, "wedged", 300) == KPB_ERR_TIMEOUT);
    elapsed = now_millis() - started;
    CHECK(elapsed >= 250 && elapsed < 1500);

    /* Both attach forms: version 2 waits for its reply, and a plain version 1
     * attach - which has no reply - waits for the first frame of the replay. */
    {
        kpb_attach_options attach;
        kpb_attach_options_init(&attach);
        started = now_millis();
        CHECK(kpb_attach_with_options_timeout(
            runtime_dir, "wedged", &attach, &connection, NULL, 300) == KPB_ERR_TIMEOUT);
        CHECK(connection.fd == -1);
        elapsed = now_millis() - started;
        CHECK(elapsed >= 250 && elapsed < 1500);
        started = now_millis();
        CHECK(kpb_attach_timeout(
            runtime_dir, "wedged", 24, 80, 0, 0, &connection, 300) == KPB_ERR_TIMEOUT);
        CHECK(connection.fd == -1);
        elapsed = now_millis() - started;
        CHECK(elapsed >= 250 && elapsed < 1500);
    }

    /* The healthy session answers a plain status and is listed; the wedged one
     * is reported as unreachable rather than dropped or waited for. */
    CHECK(kpb_query_status_timeout(runtime_dir, "healthy", &status, 300) == KPB_OK);
    memset(&tally, 0, sizeof tally);
    options.timeout_millis = 500;
    started = now_millis();
    CHECK(kpb_list_with_options(runtime_dir, &options, tally_entry, &tally) == KPB_OK);
    elapsed = now_millis() - started;
    CHECK(elapsed < 1500);
    CHECK(tally.healthy == 1 && tally_has(tally.reachable_ids, tally.healthy, "healthy"));
    CHECK(tally.unreachable == 1 && tally.timed_out == 1);
    CHECK(tally_has(tally.unreachable_ids, tally.unreachable, "wedged"));

    /* The original entry point keeps its shape: reachable sessions only. */
    memset(&tally, 0, sizeof tally);
    started = now_millis();
    {
        int seen = 0;
        CHECK(kpb_list(runtime_dir, count_session, &seen) == KPB_OK);
        CHECK(seen == 1);
    }
    elapsed = now_millis() - started;
    CHECK(elapsed < 2500);

    resume_and_end(broker, "wedged");
    terminate_and_reap("healthy");
}

/* Several stopped brokers must cost ONE deadline, not one each: the engine
 * gives `list` two seconds in total, so a per-session bound would time the
 * whole call out because of the first stuck broker. */
static void
test_list_has_one_deadline_for_many_wedged_brokers(void) {
    static const char *const wedged[] = {"wedge-a", "wedge-b", "wedge-c", "wedge-d"};
    static const char *const healthy[] = {"fine-a", "fine-b", "fine-c"};
    pid_t brokers[4];
    list_tally tally;
    kpb_list_options options;
    long started;
    long elapsed;
    size_t index;

    for (index = 0; index < 4; index++) {
        brokers[index] = spawn_sleeper(wedged[index]);
    }
    for (index = 0; index < 3; index++) (void)spawn_sleeper(healthy[index]);
    for (index = 0; index < 4; index++) CHECK(kill(brokers[index], SIGSTOP) == 0);

    memset(&tally, 0, sizeof tally);
    options.timeout_millis = 600;
    started = now_millis();
    CHECK(kpb_list_with_options(runtime_dir, &options, tally_entry, &tally) == KPB_OK);
    elapsed = now_millis() - started;
    /* Four sequential 600 ms bounds would take 2400 ms. */
    CHECK(elapsed >= 500 && elapsed < 1300);
    CHECK(tally.healthy == 3);
    CHECK(tally.unreachable == 4 && tally.timed_out == 4);
    for (index = 0; index < 3; index++) {
        CHECK(tally_has(tally.reachable_ids, tally.healthy, healthy[index]));
    }
    for (index = 0; index < 4; index++) {
        CHECK(tally_has(tally.unreachable_ids, tally.unreachable, wedged[index]));
    }

    for (index = 0; index < 4; index++) resume_and_end(brokers[index], wedged[index]);
    for (index = 0; index < 3; index++) terminate_and_reap(healthy[index]);
}

/* A runtime that does not exist is an error, not an empty listing: a typo'd
 * --runtime-dir used to look exactly like "no sessions". */
static void
test_a_missing_runtime_is_an_error_not_an_empty_list(void) {
    char scratch[64];
    char missing[KPB_PATH_MAX];
    char link_path[KPB_PATH_MAX];
    char file_path[KPB_PATH_MAX];
    int seen = 0;
    FILE *stream;
    struct stat probe;

    make_scratch_runtime(scratch, sizeof scratch);
    CHECK(snprintf(missing, sizeof missing, "%s/nope", scratch) < (int)sizeof missing);
    CHECK(kpb_check_runtime(missing) == KPB_ERR_NOT_FOUND);
    CHECK(kpb_list(missing, count_session, &seen) == KPB_ERR_NOT_FOUND);
    CHECK(seen == 0);
    /* Listing does not create what it looked for. */
    CHECK(lstat(missing, &probe) != 0 && errno == ENOENT);

    /* A runtime that exists without sessions/ is an empty listing. */
    CHECK(chmod(scratch, 0700) == 0);
    CHECK(kpb_check_runtime(scratch) == KPB_OK);
    CHECK(kpb_list(scratch, count_session, &seen) == KPB_OK);
    CHECK(seen == 0);

    /* A symlink or a non-directory is refused, not followed or listed. */
    CHECK(snprintf(link_path, sizeof link_path, "%s/link", scratch) < (int)sizeof link_path);
    CHECK(symlink(scratch, link_path) == 0);
    CHECK(kpb_check_runtime(link_path) == KPB_ERR_SECURITY);
    CHECK(kpb_list(link_path, count_session, &seen) == KPB_ERR_SECURITY);
    CHECK(snprintf(file_path, sizeof file_path, "%s/file", scratch) < (int)sizeof file_path);
    stream = fopen(file_path, "w");
    CHECK(stream != NULL);
    CHECK(fclose(stream) == 0);
    CHECK(kpb_check_runtime(file_path) == KPB_ERR_SECURITY);
    CHECK(kpb_list(file_path, count_session, &seen) == KPB_ERR_SECURITY);

    /* Not owned by the caller.  /proc is root's wherever this runs as a user. */
    if (geteuid() != 0) {
        CHECK(kpb_check_runtime("/proc") == KPB_ERR_SECURITY);
        CHECK(kpb_list("/proc", count_session, &seen) == KPB_ERR_SECURITY);
    }
    CHECK(kpb_check_runtime("relative/path") == KPB_ERR_INVALID);
    remove_tree(scratch);
}

/* The finding: an 88-character runtime failed with "invalid argument" after
 * runtime/ and sessions/ had already been created. */
static void
test_a_too_long_socket_path_is_named_and_leaves_nothing_behind(void) {
    char scratch[64];
    char runtime[KPB_PATH_MAX];
    char sessions[KPB_PATH_MAX];
    char path[KPB_PATH_MAX];
    char *command[] = {"/bin/sh", "-c", "sleep 3600", NULL};
    kpb_spawn_options options;
    kpb_status status;
    struct stat probe;
    DIR *directory;
    struct dirent *entry;
    int entries = 0;
    size_t length;

    make_scratch_runtime(scratch, sizeof scratch);
    CHECK(chmod(scratch, 0700) == 0);

    /* The runtime itself does not exist yet. */
    CHECK(snprintf(
        runtime, sizeof runtime, "%s/%s", scratch,
        "a-runtime-directory-name-long-enough-to-push-the-socket-path-over-the-limit")
        < (int)sizeof runtime);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime;
    options.session_id = "toolong";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_ERR_NAME_TOO_LONG);
    CHECK(lstat(runtime, &probe) != 0 && errno == ENOENT);

    /* Nor is anything created inside a runtime that does exist. */
    CHECK(snprintf(
        sessions, sizeof sessions, "%s/%s", scratch, "a") < (int)sizeof sessions);
    CHECK(mkdir(sessions, 0700) == 0);
    {
        char padded[KPB_PATH_MAX];
        CHECK(snprintf(
            padded, sizeof padded, "%s/%s", sessions,
            "an-existing-runtime-with-a-name-long-enough-to-be-refused-later") < (int)sizeof padded);
        CHECK(mkdir(padded, 0700) == 0);
        options.runtime_dir = padded;
        CHECK(kpb_spawn(&options, &status) == KPB_ERR_NAME_TOO_LONG);
        CHECK(snprintf(path, sizeof path, "%s/sessions", padded) < (int)sizeof path);
        CHECK(lstat(path, &probe) != 0 && errno == ENOENT);
        directory = opendir(padded);
        CHECK(directory != NULL);
        while ((entry = readdir(directory))) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) entries++;
        }
        closedir(directory);
        CHECK(entries == 0);
        CHECK(kpb_attach(padded, "toolong", 24, 80, 0, 0, &(kpb_connection){0}) ==
              KPB_ERR_NAME_TOO_LONG);
    }

    /* The reported path is the one that was refused, and the limit is exact:
     * 107 bytes binds, 108 does not. */
    CHECK(kpb_session_socket_path(runtime, "toolong", path, sizeof path) ==
          KPB_ERR_NAME_TOO_LONG);
    CHECK(strlen(path) > KPB_SOCKET_PATH_LIMIT);
    CHECK(strstr(path, "/sessions/toolong/control.sock") != NULL);
    length = strlen(scratch);
    for (entries = 0; entries < 2; entries++) {
        /* scratch + "/" + padding + "/sessions/x/control.sock" */
        size_t fixed = length + 1 + strlen("/sessions/x/control.sock");
        size_t pad = (entries == 0 ? KPB_SOCKET_PATH_LIMIT : KPB_SOCKET_PATH_LIMIT + 1) - fixed;
        CHECK(pad > 0 && pad < sizeof runtime - length - 2);
        memcpy(runtime, scratch, length);
        runtime[length] = '/';
        memset(runtime + length + 1, 'p', pad);
        runtime[length + 1 + pad] = '\0';
        CHECK(kpb_session_socket_path(runtime, "x", path, sizeof path) ==
              (entries == 0 ? KPB_OK : KPB_ERR_NAME_TOO_LONG));
        CHECK(strlen(path) == (entries == 0 ? KPB_SOCKET_PATH_LIMIT : KPB_SOCKET_PATH_LIMIT + 1));
    }
    remove_tree(scratch);
}

static int
count_session(const kpb_status *status, void *data) {
    (void)status;
    (*(int *)data)++;
    return 0;
}

/* status.cwd is where the session STARTED; the live directory is a different
 * fact, read from /proc by the caller. */
static void
test_cwd_now_follows_the_child_while_cwd_stays_the_start(void) {
    char *command[] = {"/bin/sh", "-c", "cd /usr && sleep 3600", NULL};
    kpb_spawn_options options;
    kpb_status status;
    char now[KPB_PATH_MAX];
    int attempt;

    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "cwdnow";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    for (attempt = 0; attempt < 200; attempt++) {
        if (kpb_read_cwd_now(status.child_pid, now, sizeof now) == KPB_OK &&
            strcmp(now, "/usr") == 0) {
            break;
        }
        usleep(10000);
    }
    CHECK(strcmp(now, "/usr") == 0);
    CHECK(kpb_query_status(runtime_dir, "cwdnow", &status) == KPB_OK);
    CHECK(strcmp(status.cwd, "/tmp") == 0);
    CHECK(kpb_read_cwd_now(0, now, sizeof now) == KPB_ERR_NOT_FOUND);
    CHECK(kpb_read_cwd_now(-5, now, sizeof now) == KPB_ERR_NOT_FOUND);
    CHECK(kpb_read_cwd_now(2147483000, now, sizeof now) == KPB_ERR_NOT_FOUND);
    CHECK(now[0] == '\0');
    /* A buffer too small for the answer is "unavailable", not a wrong path. */
    CHECK(kpb_read_cwd_now(status.child_pid, now, 3) == KPB_ERR_NOT_FOUND);
    terminate_and_reap("cwdnow");
}

/* --- stale proof ---------------------------------------------------------- */

static void
read_current_boot_id(char output[64]) {
    FILE *stream = fopen("/proc/sys/kernel/random/boot_id", "r");
    size_t length;
    CHECK(stream != NULL);
    CHECK(fgets(output, 64, stream) != NULL);
    fclose(stream);
    length = strlen(output);
    while (length && (output[length - 1] == '\n' || output[length - 1] == ' ')) {
        output[--length] = '\0';
    }
    CHECK(length > 0);
}

static unsigned long long
read_start_ticks(pid_t pid) {
    char path[64];
    char data[1024];
    FILE *stream;
    char *cursor;
    int field;
    CHECK(snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid) < (int)sizeof path);
    stream = fopen(path, "r");
    CHECK(stream != NULL);
    CHECK(fgets(data, sizeof data, stream) != NULL);
    fclose(stream);
    cursor = strrchr(data, ')');
    CHECK(cursor != NULL);
    cursor++;
    for (field = 3; field < 22; field++) {
        while (*cursor == ' ') cursor++;
        while (*cursor && *cursor != ' ') cursor++;
    }
    while (*cursor == ' ') cursor++;
    return strtoull(cursor, NULL, 10);
}

/* A session directory with no broker behind it, holding whatever metadata and
 * journal a test wants to describe.  Returns the directory path. */
static void
make_fake_session(
    const char *runtime,
    const char *session_id,
    const char *metadata,
    const char *journal,
    char directory[KPB_PATH_MAX]
) {
    char sessions[KPB_PATH_MAX];
    char path[KPB_PATH_MAX];
    FILE *stream;
    CHECK(snprintf(sessions, sizeof sessions, "%s/sessions", runtime) < (int)sizeof sessions);
    if (mkdir(sessions, 0700) != 0) CHECK(errno == EEXIST);
    CHECK(snprintf(directory, KPB_PATH_MAX, "%s/%s", sessions, session_id) < KPB_PATH_MAX);
    CHECK(mkdir(directory, 0700) == 0);
    if (metadata) {
        CHECK(snprintf(path, sizeof path, "%s/metadata", directory) < (int)sizeof path);
        stream = fopen(path, "w");
        CHECK(stream != NULL);
        CHECK(fputs(metadata, stream) >= 0);
        CHECK(fclose(stream) == 0);
    }
    if (journal) {
        CHECK(snprintf(path, sizeof path, "%s/journal.bin", directory) < (int)sizeof path);
        stream = fopen(path, "w");
        CHECK(stream != NULL);
        CHECK(fputs(journal, stream) >= 0);
        CHECK(fclose(stream) == 0);
    }
}

/* A pid that is provably gone: a child this process has already reaped. */
static pid_t
dead_pid(void) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) _exit(0);
    CHECK(waitpid(child, NULL, 0) == child);
    return child;
}

static bool
exists(const char *path) {
    struct stat probe;
    return lstat(path, &probe) == 0;
}

/* The pid is this very process, which is as alive as anything gets, so only the
 * boot id or the start time can show the recorded broker is somebody else. */
static void
test_stale_proof_uses_boot_id_and_start_ticks(void) {
    char scratch[64];
    char boot[64];
    char other_boot[64];
    char metadata[1024];
    char directory[KPB_PATH_MAX];
    unsigned long long ticks = read_start_ticks(getpid());
    int seen = 0;
    int index;
    struct {
        const char *id;
        const char *boot;      /* NULL: line omitted */
        long long ticks;       /* < 0: line omitted */
        bool reaped;
    } cases[] = {
        {"other-boot", "OTHER", (long long)ticks, true},
        {"other-boot-no-ticks", "OTHER", -1, true},
        {"reused-pid", "SAME", (long long)ticks + 1000, true},
        {"reused-pid-no-boot", NULL, (long long)ticks + 1000, true},
        {"same-process", "SAME", (long long)ticks, false},
        {"boot-only", "SAME", -1, false},
        {"ticks-only", NULL, (long long)ticks, false},
        {"pid-only", NULL, -1, false},
    };

    make_scratch_runtime(scratch, sizeof scratch);
    CHECK(chmod(scratch, 0700) == 0);
    read_current_boot_id(boot);
    memcpy(other_boot, boot, sizeof boot);
    other_boot[0] = other_boot[0] == '0' ? '1' : '0';
    CHECK(strcmp(boot, other_boot) != 0);

    for (index = 0; index < (int)(sizeof cases / sizeof cases[0]); index++) {
        size_t used = (size_t)snprintf(
            metadata, sizeof metadata,
            "version=1\nid=%s\nbroker_pid=%ld\nchild_pid=1\nstarted_millis=1\n",
            cases[index].id, (long)getpid());
        if (cases[index].boot) {
            used += (size_t)snprintf(
                metadata + used, sizeof metadata - used, "boot_id=%s\n",
                strcmp(cases[index].boot, "SAME") == 0 ? boot : other_boot);
        }
        if (cases[index].ticks >= 0) {
            used += (size_t)snprintf(
                metadata + used, sizeof metadata - used, "start_ticks=%lld\n",
                cases[index].ticks);
        }
        make_fake_session(scratch, cases[index].id, metadata, NULL, directory);
    }
    /* Malformed identity fields are absent evidence, never positive proof. */
    make_fake_session(
        scratch, "garbled",
        "version=1\nid=garbled\nbroker_pid=1\nchild_pid=1\nstarted_millis=1\n"
        "boot_id=\nstart_ticks=notanumber\n",
        NULL, directory);
    CHECK(snprintf(
        metadata, sizeof metadata,
        "version=1\nid=garbled-live\nbroker_pid=%ld\nchild_pid=1\nstarted_millis=1\n"
        "boot_id=\nstart_ticks=12x\n", (long)getpid()) < (int)sizeof metadata);
    make_fake_session(scratch, "garbled-live", metadata, NULL, directory);

    CHECK(kpb_list(scratch, count_session, &seen) == KPB_OK);
    CHECK(seen == 0);
    for (index = 0; index < (int)(sizeof cases / sizeof cases[0]); index++) {
        CHECK(snprintf(
            directory, sizeof directory, "%s/sessions/%s", scratch, cases[index].id)
            < (int)sizeof directory);
        CHECK(exists(directory) == !cases[index].reaped);
    }
    CHECK(snprintf(directory, sizeof directory, "%s/sessions/garbled-live", scratch)
          < (int)sizeof directory);
    CHECK(exists(directory));
    /* pid 1 exists and the garbled identity cannot say otherwise. */
    CHECK(snprintf(directory, sizeof directory, "%s/sessions/garbled", scratch)
          < (int)sizeof directory);
    CHECK(exists(directory));

    /* A reaped ID is spawnable again, a live-looking one is not. */
    {
        char *command[] = {"/bin/sh", "-c", "sleep 3600", NULL};
        kpb_spawn_options options;
        kpb_spawn_options_init(&options);
        options.runtime_dir = scratch;
        options.cwd = "/tmp";
        options.argv = command;
        options.session_id = "reused-pid";
        CHECK(kpb_spawn(&options, NULL) == KPB_OK);
        CHECK(kpb_terminate(scratch, "reused-pid") == KPB_OK);
        options.session_id = "same-process";
        CHECK(kpb_spawn(&options, NULL) == KPB_ERR_EXISTS);
    }
    {
        int attempt;
        for (attempt = 0; attempt < 100; attempt++) {
            kpb_status status;
            if (kpb_query_status(scratch, "reused-pid", &status) == KPB_ERR_NOT_FOUND) break;
            usleep(20000);
        }
    }
    remove_tree(scratch);
}

/* A real broker records the identity a reaper will later prove against, and a
 * list walk never reaps it. */
static void
test_a_live_broker_records_its_identity_and_is_never_reaped(void) {
    char metadata_path[KPB_PATH_MAX];
    char data[2048];
    char expected[160];
    char boot[64];
    kpb_status status;
    FILE *stream;
    size_t size;
    int seen = 0;

    (void)spawn_sleeper("identity");
    CHECK(kpb_query_status(runtime_dir, "identity", &status) == KPB_OK);
    CHECK(snprintf(
        metadata_path, sizeof metadata_path, "%s/sessions/identity/metadata",
        runtime_dir) < (int)sizeof metadata_path);
    stream = fopen(metadata_path, "r");
    CHECK(stream != NULL);
    size = fread(data, 1, sizeof data - 1, stream);
    fclose(stream);
    data[size] = '\0';
    read_current_boot_id(boot);
    CHECK(snprintf(expected, sizeof expected, "\nboot_id=%s\n", boot) < (int)sizeof expected);
    CHECK(strstr(data, expected) != NULL);
    CHECK(snprintf(
        expected, sizeof expected, "\nstart_ticks=%llu\n",
        read_start_ticks(status.broker_pid)) < (int)sizeof expected);
    CHECK(strstr(data, expected) != NULL);

    /* Even a stopped broker, which answers nothing, is not a corpse. */
    CHECK(kill(status.broker_pid, SIGSTOP) == 0);
    {
        kpb_list_options options = {.timeout_millis = 200};
        list_tally tally;
        memset(&tally, 0, sizeof tally);
        CHECK(kpb_list_with_options(runtime_dir, &options, tally_entry, &tally) == KPB_OK);
        CHECK(tally.unreachable == 1);
    }
    CHECK(exists(metadata_path));
    CHECK(kill(status.broker_pid, SIGCONT) == 0);
    CHECK(kpb_list(runtime_dir, count_session, &seen) == KPB_OK);
    CHECK(seen == 1);
    terminate_and_reap("identity");
}

/* --- the reaped archive --------------------------------------------------- */

static void
set_reaped_limits(const char *bytes, const char *files) {
    if (bytes) CHECK(setenv("KITTY_PTY_BROKER_REAPED_MAX_BYTES", bytes, 1) == 0);
    else unsetenv("KITTY_PTY_BROKER_REAPED_MAX_BYTES");
    if (files) CHECK(setenv("KITTY_PTY_BROKER_REAPED_MAX_FILES", files, 1) == 0);
    else unsetenv("KITTY_PTY_BROKER_REAPED_MAX_FILES");
}

static mode_t
mode_of(const char *path) {
    struct stat probe;
    CHECK(lstat(path, &probe) == 0);
    return probe.st_mode & 07777;
}

static size_t
read_text(const char *path, char *buffer, size_t capacity) {
    FILE *stream = fopen(path, "r");
    size_t size;
    CHECK(stream != NULL);
    size = fread(buffer, 1, capacity - 1, stream);
    fclose(stream);
    buffer[size] = '\0';
    return size;
}

typedef struct {
    int count;
    kpb_reaped_entry first;
    kpb_reaped_entry last;
} reaped_tally;

static int
tally_reaped(const kpb_reaped_entry *entry, void *data) {
    reaped_tally *tally = data;
    if (tally->count == 0) tally->first = *entry;
    tally->last = *entry;
    tally->count++;
    return 0;
}

/* End to end: a broker killed outright leaves its screen behind. */
static void
test_reaping_archives_the_journal_instead_of_deleting_it(void) {
    char scratch[64];
    char *command[] = {"/bin/sh", "-c", "printf ARCHIVE_ME; sleep 3600", NULL};
    char reaped_dir[KPB_PATH_MAX];
    char journal[KPB_PATH_MAX];
    char meta[KPB_PATH_MAX];
    char session_dir[KPB_PATH_MAX];
    char found[KPB_PATH_MAX];
    char text[4096];
    kpb_spawn_options options;
    kpb_status status;
    reaped_tally tally;
    int attempt;

    set_reaped_limits(NULL, NULL);
    make_scratch_runtime(scratch, sizeof scratch);
    CHECK(chmod(scratch, 0700) == 0);
    kpb_spawn_options_init(&options);
    options.runtime_dir = scratch;
    options.session_id = "dies.badly";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    for (attempt = 0; attempt < 300; attempt++) {
        CHECK(kpb_query_status(scratch, "dies.badly", &status) == KPB_OK);
        if (status.journal_bytes >= 10) break;
        usleep(10000);
    }
    CHECK(status.journal_bytes >= 10);
    CHECK(kill(status.broker_pid, SIGKILL) == 0);
    CHECK(waitpid(status.broker_pid, NULL, 0) == status.broker_pid);
    (void)kill(status.child_pid, SIGKILL);

    CHECK(snprintf(session_dir, sizeof session_dir, "%s/sessions/dies.badly", scratch)
          < (int)sizeof session_dir);
    CHECK(snprintf(reaped_dir, sizeof reaped_dir, "%s/reaped", scratch) < (int)sizeof reaped_dir);
    CHECK(snprintf(
        journal, sizeof journal, "%s/dies.badly.%llu.journal", reaped_dir,
        (unsigned long long)status.started_millis) < (int)sizeof journal);
    CHECK(snprintf(
        meta, sizeof meta, "%s/dies.badly.%llu.meta", reaped_dir,
        (unsigned long long)status.started_millis) < (int)sizeof meta);

    CHECK(kpb_list(scratch, count_session, &attempt) == KPB_OK);
    CHECK(!exists(session_dir));
    CHECK(mode_of(reaped_dir) == 0700);
    CHECK(mode_of(journal) == 0600);
    CHECK(mode_of(meta) == 0600);
    CHECK(read_text(journal, text, sizeof text) >= 10);
    CHECK(strstr(text, "ARCHIVE_ME") != NULL);
    (void)read_text(meta, text, sizeof text);
    CHECK(strstr(text, "id=dies.badly\n") != NULL);
    CHECK(strstr(text, "broker_pid=") != NULL);
    CHECK(strstr(text, "\nreaped_millis=") != NULL);

    memset(&tally, 0, sizeof tally);
    CHECK(kpb_list_reaped(scratch, tally_reaped, &tally) == KPB_OK);
    CHECK(tally.count == 1);
    CHECK(strcmp(tally.first.session_id, "dies.badly") == 0);
    CHECK(tally.first.started_millis == status.started_millis);
    CHECK(tally.first.reaped_millis > 0);
    CHECK(tally.first.journal_bytes >= 10);
    CHECK(strcmp(tally.first.journal_path, journal) == 0);
    CHECK(strcmp(tally.first.meta_path, meta) == 0);
    CHECK(kpb_reaped_path(scratch, "dies.badly", found, sizeof found) == KPB_OK);
    CHECK(strcmp(found, journal) == 0);
    CHECK(kpb_reaped_path(scratch, "other", found, sizeof found) == KPB_ERR_NOT_FOUND);
    CHECK(found[0] == '\0');

    /* The ID is spawnable again and a second death archives a second journal
     * rather than overwriting the first. */
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    for (attempt = 0; attempt < 300; attempt++) {
        CHECK(kpb_query_status(scratch, "dies.badly", &status) == KPB_OK);
        if (status.journal_bytes >= 10) break;
        usleep(10000);
    }
    CHECK(kill(status.broker_pid, SIGKILL) == 0);
    CHECK(waitpid(status.broker_pid, NULL, 0) == status.broker_pid);
    (void)kill(status.child_pid, SIGKILL);
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    memset(&tally, 0, sizeof tally);
    CHECK(kpb_list_reaped(scratch, tally_reaped, &tally) == KPB_OK);
    CHECK(tally.count == 2);
    CHECK(tally.first.started_millis <= tally.last.started_millis);
    CHECK(kpb_reaped_path(scratch, "dies.badly", found, sizeof found) == KPB_OK);
    CHECK(strcmp(found, tally.last.journal_path) == 0);
    CHECK(kpb_terminate(scratch, "dies.badly") == KPB_OK);
    for (attempt = 0; attempt < 100; attempt++) {
        if (kpb_query_status(scratch, "dies.badly", &status) == KPB_ERR_NOT_FOUND) break;
        usleep(20000);
    }
    remove_tree(scratch);
}

static void
reap_fake(const char *runtime, const char *id, unsigned long long started, const char *journal) {
    char metadata[256];
    char directory[KPB_PATH_MAX];
    int seen = 0;
    CHECK(snprintf(
        metadata, sizeof metadata,
        "version=1\nid=%s\nbroker_pid=%ld\nchild_pid=1\nstarted_millis=%llu\n",
        id, (long)dead_pid(), started) < (int)sizeof metadata);
    make_fake_session(runtime, id, metadata, journal, directory);
    CHECK(kpb_list(runtime, count_session, &seen) == KPB_OK);
    CHECK(!exists(directory));
}

/* The broker bounds its own archive, so a standalone install never grows
 * without limit; the oldest goes first. */
static void
test_the_reaped_archive_is_bounded_oldest_first(void) {
    char scratch[64];
    char path[KPB_PATH_MAX];
    char journal[400];
    reaped_tally tally;
    int index;

    memset(journal, 'j', sizeof journal - 1);
    journal[sizeof journal - 1] = '\0';

    /* By count. */
    make_scratch_runtime(scratch, sizeof scratch);
    CHECK(chmod(scratch, 0700) == 0);
    set_reaped_limits(NULL, "2");
    reap_fake(scratch, "first", 1000, journal);
    usleep(30000);
    reap_fake(scratch, "second", 2000, journal);
    usleep(30000);
    reap_fake(scratch, "third", 3000, journal);
    memset(&tally, 0, sizeof tally);
    CHECK(kpb_list_reaped(scratch, tally_reaped, &tally) == KPB_OK);
    CHECK(tally.count == 2);
    CHECK(strcmp(tally.first.session_id, "second") == 0);
    CHECK(strcmp(tally.last.session_id, "third") == 0);
    CHECK(snprintf(path, sizeof path, "%s/reaped/first.1000.journal", scratch) < (int)sizeof path);
    CHECK(!exists(path));
    CHECK(snprintf(path, sizeof path, "%s/reaped/first.1000.meta", scratch) < (int)sizeof path);
    CHECK(!exists(path));
    remove_tree(scratch);

    /* By bytes: each archive is about 400 bytes of journal plus its .meta. */
    make_scratch_runtime(scratch, sizeof scratch);
    CHECK(chmod(scratch, 0700) == 0);
    set_reaped_limits("1200", NULL);
    for (index = 1; index <= 4; index++) {
        char id[16];
        CHECK(snprintf(id, sizeof id, "n%d", index) < (int)sizeof id);
        reap_fake(scratch, id, (unsigned long long)index * 1000ULL, journal);
        usleep(30000);
    }
    memset(&tally, 0, sizeof tally);
    CHECK(kpb_list_reaped(scratch, tally_reaped, &tally) == KPB_OK);
    CHECK(tally.count >= 1 && tally.count < 4);
    CHECK(strcmp(tally.last.session_id, "n4") == 0);
    CHECK(strcmp(tally.first.session_id, "n1") != 0);
    {
        uint64_t total = 0;
        struct stat probe;
        DIR *directory;
        struct dirent *entry;
        CHECK(snprintf(path, sizeof path, "%s/reaped", scratch) < (int)sizeof path);
        directory = opendir(path);
        CHECK(directory != NULL);
        while ((entry = readdir(directory))) {
            char full[KPB_PATH_MAX];
            if (entry->d_name[0] == '.') continue;
            CHECK(snprintf(full, sizeof full, "%s/%s", path, entry->d_name) < (int)sizeof full);
            CHECK(lstat(full, &probe) == 0);
            total += (uint64_t)probe.st_size;
        }
        closedir(directory);
        CHECK(total <= 1200);
    }
    remove_tree(scratch);

    /* Default bounds are what the header says. */
    CHECK(KPB_DEFAULT_REAPED_MAX_BYTES == 256ULL * 1024ULL * 1024ULL);
    CHECK(KPB_DEFAULT_REAPED_MAX_FILES == 64U);
    set_reaped_limits(NULL, NULL);
}

/* Archiving is best-effort and must never turn into a corpse that blocks its
 * own ID: whatever cannot be kept is deleted as it always was. */
static void
test_archiving_failures_still_reap_the_session(void) {
    char scratch[64];
    char elsewhere[KPB_PATH_MAX];
    char target[KPB_PATH_MAX];
    char link_path[KPB_PATH_MAX];
    char directory[KPB_PATH_MAX];
    char text[256];
    char metadata[256];
    reaped_tally tally;
    int seen = 0;

    set_reaped_limits(NULL, NULL);
    make_scratch_runtime(scratch, sizeof scratch);
    CHECK(chmod(scratch, 0700) == 0);

    /* An empty journal holds nothing worth keeping. */
    reap_fake(scratch, "empty", 5, "");
    CHECK(snprintf(directory, sizeof directory, "%s/reaped", scratch) < (int)sizeof directory);
    CHECK(!exists(directory));

    /* A journal that is a symlink is never followed, and the target survives. */
    CHECK(snprintf(target, sizeof target, "%s/precious", scratch) < (int)sizeof target);
    {
        FILE *stream = fopen(target, "w");
        CHECK(stream != NULL);
        CHECK(fputs("precious", stream) >= 0);
        CHECK(fclose(stream) == 0);
    }
    CHECK(snprintf(
        metadata, sizeof metadata,
        "version=1\nid=linked\nbroker_pid=%ld\nchild_pid=1\nstarted_millis=7\n",
        (long)dead_pid()) < (int)sizeof metadata);
    make_fake_session(scratch, "linked", metadata, NULL, directory);
    CHECK(snprintf(link_path, sizeof link_path, "%s/journal.bin", directory) < (int)sizeof link_path);
    CHECK(symlink(target, link_path) == 0);
    CHECK(kpb_list(scratch, count_session, &seen) == KPB_OK);
    CHECK(!exists(directory));
    CHECK(read_text(target, text, sizeof text) == 8);
    memset(&tally, 0, sizeof tally);
    CHECK(kpb_list_reaped(scratch, tally_reaped, &tally) == KPB_OK);
    CHECK(tally.count == 0);

    /* A reaped/ that is a symlink is not written through. */
    CHECK(snprintf(elsewhere, sizeof elsewhere, "%s/elsewhere", scratch) < (int)sizeof elsewhere);
    CHECK(mkdir(elsewhere, 0700) == 0);
    CHECK(snprintf(directory, sizeof directory, "%s/reaped", scratch) < (int)sizeof directory);
    CHECK(symlink(elsewhere, directory) == 0);
    reap_fake(scratch, "diverted", 9, "journal bytes");
    {
        DIR *listing = opendir(elsewhere);
        struct dirent *entry;
        int entries = 0;
        CHECK(listing != NULL);
        while ((entry = readdir(listing))) {
            if (entry->d_name[0] != '.') entries++;
        }
        closedir(listing);
        CHECK(entries == 0);
    }
    CHECK(kpb_list_reaped(scratch, tally_reaped, &tally) == KPB_ERR_SECURITY);
    remove_tree(scratch);
}


static void
cli_path_beside_test(char *cli_path, size_t capacity) {
    const char *slash = strrchr(test_program_path, '/');
    size_t directory_size;
    CHECK(slash != NULL);
    directory_size = (size_t)(slash - test_program_path);
    CHECK(directory_size + sizeof "/kitty-pty-broker" < capacity);
    memcpy(cli_path, test_program_path, directory_size);
    memcpy(cli_path + directory_size, "/kitty-pty-broker", sizeof "/kitty-pty-broker");
}

/* `o` watches a pane read-only - including one that is already attached, which
 * Enter refuses - and Ctrl-] returns to the list rather than ending the TUI.
 * Keys typed while watching must not reach the pane. */
static void
test_tui_observes_an_attached_pane_and_returns(void) {
    char *command[] = {
        "/bin/sh", "-c", "printf OBSERVED_TEXT; stty -echo; cat > /dev/null", NULL};
    char cli_path[KPB_PATH_MAX];
    kpb_spawn_options options;
    kpb_status status;
    kpb_connection client;
    struct winsize size = {.ws_row = 24, .ws_col = 100};
    unsigned char output[65536];
    unsigned char after[65536];
    size_t used = 0;
    size_t later = 0;
    int master;
    int wait_status;
    pid_t child;

    cli_path_beside_test(cli_path, sizeof cli_path);
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = "tui-observe";
    options.cwd = "/tmp";
    options.argv = command;
    CHECK(kpb_spawn(&options, &status) == KPB_OK);
    CHECK(attach_v2("tui-observe", &client, NULL, 0, 0, 0) == KPB_OK);
    {
        unsigned char seen[4096];
        size_t seen_size = 0;
        while (!memmem(seen, seen_size, "OBSERVED_TEXT", 13)) {
            kpb_event event;
            wait_readable(client.fd);
            CHECK(kpb_receive(&client, seen + seen_size, sizeof seen - seen_size, &event) == KPB_OK);
            if (event.type == KPB_EVENT_OUTPUT) seen_size += event.size;
            CHECK(seen_size < sizeof seen);
        }
    }
    CHECK(kpb_query_status(runtime_dir, "tui-observe", &status) == KPB_OK);
    CHECK(status.attached);

    child = forkpty(&master, NULL, NULL, &size);
    CHECK(child >= 0);
    if (child == 0) {
        execl(cli_path, cli_path, "--runtime-dir", runtime_dir, "--timeout", "1.5",
              "tui", (char *)NULL);
        _exit(127);
    }
    used = read_pty_until(master, output, used, sizeof output, "tui-observe");
    used = read_pty_until(master, output, used, sizeof output, "attached");
    /* The key help names observe, and the header names the runtime and bound. */
    used = read_pty_until(master, output, used, sizeof output, "o observe");
    used = read_pty_until(master, output, used, sizeof output, "TIMEOUT 1.5s");
    used = read_pty_until(master, output, used, sizeof output, runtime_dir);

    /* Enter on an attached pane is refused and says to observe instead. */
    CHECK(write(master, "\r", 1) == 1);
    used = read_pty_until(master, output, used, sizeof output, "already attached");

    CHECK(write(master, "o", 1) == 1);
    used = read_pty_until(master, output, used, sizeof output, "OBSERVED_TEXT");
    /* `x` would end the session if it reached the TUI's key handling, and
     * anything typed here must never reach the pane. */
    CHECK(write(master, "xyz\r", 4) == 4);
    usleep(200000);
    CHECK(kpb_query_status(runtime_dir, "tui-observe", &status) == KPB_OK);
    CHECK(status.attached);
    CHECK(write(master, "\x1d", 1) == 1);
    while (later == 0 || !memmem(after, later, "PTY SESSIONS", 12)) {
        ssize_t count;
        wait_readable(master);
        count = read(master, after + later, sizeof after - later);
        CHECK(count > 0);
        later += (size_t)count;
        CHECK(later < sizeof after);
    }
    /* Back at the list, the attached client is still attached and the session
     * still runs. */
    CHECK(kpb_query_status(runtime_dir, "tui-observe", &status) == KPB_OK);
    CHECK(status.attached);
    CHECK(write(master, "q", 1) == 1);
    CHECK(waitpid(child, &wait_status, 0) == child);
    CHECK(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0);
    close(master);
    kpb_detach(&client);
    terminate_and_reap("tui-observe");
}

/* A broker that does not answer is shown, not hidden, and cannot be attached
 * or observed from the list. */
static void
test_tui_shows_an_unreachable_session(void) {
    char cli_path[KPB_PATH_MAX];
    struct winsize size = {.ws_row = 24, .ws_col = 100};
    unsigned char output[65536];
    size_t used = 0;
    int master;
    int wait_status;
    pid_t child;
    pid_t broker;

    cli_path_beside_test(cli_path, sizeof cli_path);
    broker = spawn_sleeper("tui-wedged");
    CHECK(kill(broker, SIGSTOP) == 0);
    child = forkpty(&master, NULL, NULL, &size);
    CHECK(child >= 0);
    if (child == 0) {
        execl(cli_path, cli_path, "--runtime-dir", runtime_dir, "--timeout", "0.3",
              "tui", (char *)NULL);
        _exit(127);
    }
    used = read_pty_until(master, output, used, sizeof output, "tui-wedged");
    used = read_pty_until(master, output, used, sizeof output, "unreachable");
    used = read_pty_until(master, output, used, sizeof output, "1 UNREACHABLE");
    CHECK(write(master, "\r", 1) == 1);
    used = read_pty_until(master, output, used, sizeof output, "cannot be attached");
    CHECK(write(master, "o", 1) == 1);
    used = read_pty_until(master, output, used, sizeof output, "cannot be observed");
    CHECK(write(master, "q", 1) == 1);
    CHECK(waitpid(child, &wait_status, 0) == child);
    CHECK(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0);
    close(master);
    resume_and_end(broker, "tui-wedged");
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
    if (argc == 2 && strcmp(argv[1], "--gated-writer-child") == 0) {
        return gated_writer_child();
    }
    if (argc == 2 && strcmp(argv[1], "--finishing-writer-child") == 0) {
        return finishing_writer_child();
    }
    if (argc == 3 && strcmp(argv[1], "--descriptor-child") == 0) {
        return descriptor_child(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "--only") == 0) only_test = argv[2];
    CHECK(realpath(argv[0], test_program_path) != NULL);
    test_program = test_program_path;
    CHECK(mkdtemp(runtime_dir) != NULL);
    CHECK(chmod(runtime_dir, 0700) == 0);
    RUN(test_ids);
    CHECK(kpb_prepare_runtime(runtime_dir) == KPB_OK);
    RUN(test_spawn_detach_replay_and_exit);
    RUN(test_transcript_elides_graphics);
    RUN(test_transcript_keeps_graphics_when_asked);
    RUN(test_transcript_scanner_boundaries_and_dense_markers);
    RUN(test_existing_transcript_is_private_and_bounded_immediately);
    RUN(test_transcript_rotates_and_keeps_newest);
    RUN(test_transcript_records_pane_size_and_resizes);
    RUN(test_transcript_captures_output_written_just_before_exit);
    RUN(test_transcript_absent_by_default);
    RUN(test_large_input_backpressure);
    RUN(test_inherited_descriptors_are_command_only);
    RUN(test_child_signal_mask_and_wait_status);
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
    RUN(test_receive_reports_a_skipped_oversized_frame);
    RUN(test_skipped_output_is_recoverable_by_resume);
    RUN(test_observer_replay_truncation_is_flagged);
    RUN(test_tui);
    RUN(test_a_stalled_client_does_not_stop_the_broker);
    RUN(test_a_write_stalled_client_does_not_stop_the_broker);
    RUN(test_slow_reader_keeps_replay_tail_and_exit);
    RUN(test_terminate);
    RUN(test_dead_session_directory_is_reaped);
    RUN(test_reaping_never_removes_a_live_session);
    RUN(test_a_stopped_broker_does_not_hang_clients);
    RUN(test_list_has_one_deadline_for_many_wedged_brokers);
    RUN(test_a_missing_runtime_is_an_error_not_an_empty_list);
    RUN(test_a_too_long_socket_path_is_named_and_leaves_nothing_behind);
    RUN(test_cwd_now_follows_the_child_while_cwd_stays_the_start);
    RUN(test_stale_proof_uses_boot_id_and_start_ticks);
    RUN(test_a_live_broker_records_its_identity_and_is_never_reaped);
    RUN(test_reaping_archives_the_journal_instead_of_deleting_it);
    RUN(test_the_reaped_archive_is_bounded_oldest_first);
    RUN(test_archiving_failures_still_reap_the_session);
    RUN(test_tui_observes_an_attached_pane_and_returns);
    RUN(test_tui_shows_an_unreachable_session);
    {
        char sessions[4096];
        snprintf(sessions, sizeof sessions, "%s/sessions", runtime_dir);
        CHECK(rmdir(sessions) == 0);
        CHECK(rmdir(runtime_dir) == 0);
    }
    if (only_test && tests_run == 0) {
        fprintf(stderr, "no test named %s\n", only_test);
        return 1;
    }
    puts("all kitty-pty-broker tests passed");
    return 0;
}
