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
    test_large_input_backpressure();
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
