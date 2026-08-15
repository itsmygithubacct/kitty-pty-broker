#define _GNU_SOURCE

#include "kitty_pty_broker.h"
#include "internal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define BENCHMARK_DEFAULT_BYTES (16U * 1024U * 1024U)
#define BENCHMARK_DEFAULT_SAMPLES 7U
#define BENCHMARK_SAMPLE_MAX 31U
#define BENCHMARK_READY "KPB-BENCH-READY\n"

typedef enum {
    BENCHMARK_PLAIN = 0,
    BENCHMARK_TRANSCRIPT_KEEP,
    BENCHMARK_TRANSCRIPT_TEXT,
    BENCHMARK_TRANSCRIPT_GRAPHICS,
    BENCHMARK_REPLAY
} benchmark_mode;

static void
fail(const char *operation) {
    fprintf(stderr, "benchmark-broker: %s: %s\n", operation, strerror(errno));
    exit(1);
}

static void
require(bool condition, const char *operation) {
    if (!condition) {
        if (!errno) errno = EINVAL;
        fail(operation);
    }
}

static uint64_t
monotonic_nanos(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) fail("clock_gettime");
    return (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
}

static int
wait_for_trigger(void) {
    unsigned char trigger;
    while (true) {
        ssize_t count = read(STDIN_FILENO, &trigger, 1);
        if (count == 1) return 0;
        if (count < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int
producer(size_t bytes, bool graphics, bool replay) {
    static unsigned char block[64U * 1024U];
    struct termios attributes;
    size_t produced = 0;
    memset(block, 'x', sizeof block);
    if (tcgetattr(STDIN_FILENO, &attributes) != 0) return 2;
    cfmakeraw(&attributes);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) != 0) return 2;
    if (!replay) {
        if (write_all_fd(
                STDOUT_FILENO, BENCHMARK_READY,
                sizeof BENCHMARK_READY - 1U) < 0) {
            return 2;
        }
        if (wait_for_trigger() != 0) return 2;
    }
    if (graphics && write_all_fd(STDOUT_FILENO, "\033_G", 3) < 0) return 2;
    while (produced < bytes) {
        size_t wanted = bytes - produced;
        if (wanted > sizeof block) wanted = sizeof block;
        if (write_all_fd(STDOUT_FILENO, block, wanted) < 0) return 2;
        produced += wanted;
    }
    if (graphics && write_all_fd(STDOUT_FILENO, "\033\\", 2) < 0) return 2;
    if (replay && wait_for_trigger() != 0) return 2;
    return 0;
}

static int
wait_readable(int fd) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
    int result;
    do {
        result = poll(&descriptor, 1, 30000);
    } while (result < 0 && errno == EINTR);
    return result > 0 ? 0 : -1;
}

static void
wait_until_ready(kpb_connection *connection) {
    unsigned char buffer[KPB_IO_CHUNK];
    size_t ready = 0;
    bool replay_done = false;
    while (!replay_done || ready < sizeof BENCHMARK_READY - 1U) {
        kpb_event event;
        require(wait_readable(connection->fd) == 0, "wait for producer");
        require(
            kpb_receive(connection, buffer, sizeof buffer, &event) == KPB_OK,
            "receive producer readiness");
        if (event.type == KPB_EVENT_REPLAY_DONE) {
            replay_done = true;
            continue;
        }
        require(event.type == KPB_EVENT_OUTPUT, "unexpected readiness event");
        require(
            event.size <= sizeof BENCHMARK_READY - 1U - ready,
            "excess readiness output");
        require(
            memcmp(BENCHMARK_READY + ready, buffer, event.size) == 0,
            "invalid readiness output");
        ready += event.size;
    }
}

static const char *
mode_name(benchmark_mode mode) {
    switch (mode) {
        case BENCHMARK_PLAIN: return "live output";
        case BENCHMARK_TRANSCRIPT_KEEP: return "transcript keep";
        case BENCHMARK_TRANSCRIPT_TEXT: return "transcript text elision scan";
        case BENCHMARK_TRANSCRIPT_GRAPHICS: return "transcript graphics elision";
        case BENCHMARK_REPLAY: return "reattach replay";
    }
    return "unknown";
}

static uint64_t
run_sample(
    const char *program,
    const char *runtime_dir,
    benchmark_mode mode,
    size_t bytes,
    unsigned int sample
) {
    char byte_text[32];
    char graphics_text[2];
    char session_id[64];
    char transcript_path[KPB_PATH_MAX];
    char replay_text[2];
    char *command[6];
    kpb_spawn_options options;
    kpb_connection connection;
    kpb_status status;
    unsigned char buffer[KPB_IO_CHUNK];
    const bool graphics = mode == BENCHMARK_TRANSCRIPT_GRAPHICS;
    const bool replay = mode == BENCHMARK_REPLAY;
    const size_t expected = bytes + (graphics ? 5U : 0U);
    size_t received = 0;
    uint64_t started;
    uint64_t finished = 0;
    int child_status = -1;
    int broker_status;
    pid_t waited;

    require(
        snprintf(byte_text, sizeof byte_text, "%zu", bytes) > 0,
        "format byte count");
    graphics_text[0] = graphics ? '1' : '0';
    graphics_text[1] = '\0';
    replay_text[0] = replay ? '1' : '0';
    replay_text[1] = '\0';
    require(
        snprintf(session_id, sizeof session_id, "bench-%u-%u", (unsigned int)mode, sample) > 0,
        "format session id");
    require(
        snprintf(
            transcript_path, sizeof transcript_path,
            "%s/transcript-%u-%u.log", runtime_dir,
            (unsigned int)mode, sample) > 0,
        "format transcript path");

    command[0] = (char *)program;
    command[1] = "--producer";
    command[2] = byte_text;
    command[3] = graphics_text;
    command[4] = replay_text;
    command[5] = NULL;
    kpb_spawn_options_init(&options);
    options.runtime_dir = runtime_dir;
    options.session_id = session_id;
    options.cwd = "/tmp";
    options.argv = command;
    options.journal_limit = 0;
    if (mode != BENCHMARK_PLAIN && !replay) {
        options.transcript_path = transcript_path;
        options.transcript_limit = 0;
        options.transcript_graphics = mode == BENCHMARK_TRANSCRIPT_KEEP
            ? KPB_TRANSCRIPT_GRAPHICS_KEEP : KPB_TRANSCRIPT_GRAPHICS_ELIDE;
    }
    require(kpb_spawn(&options, &status) == KPB_OK, "spawn producer");
    if (replay) {
        unsigned int attempt;
        for (attempt = 0; attempt < 3000U; attempt++) {
            kpb_status current;
            if (kpb_query_status(runtime_dir, session_id, &current) == KPB_OK &&
                current.journal_bytes == expected) {
                break;
            }
            usleep(1000);
        }
        require(attempt < 3000U, "wait for journal");
    }
    started = monotonic_nanos();
    require(
        kpb_attach(runtime_dir, session_id, 24, 80, 0, 0, &connection) == KPB_OK,
        "attach producer");
    if (!replay) {
        wait_until_ready(&connection);
        started = monotonic_nanos();
        require(kpb_send_input(&connection, "x", 1) == KPB_OK, "start producer");
    }
    while (child_status < 0) {
        kpb_event event;
        require(wait_readable(connection.fd) == 0, "wait for output");
        require(
            kpb_receive(&connection, buffer, sizeof buffer, &event) == KPB_OK,
            "receive output");
        if (event.type == KPB_EVENT_OUTPUT) {
            if (received > expected || event.size > expected - received) {
                size_t shown = event.size < 32U ? event.size : 32U;
                size_t index;
                fprintf(
                    stderr,
                    "benchmark-broker: excess output: received=%zu event=%zu "
                    "expected=%zu prefix=",
                    received, event.size, expected);
                for (index = 0; index < shown; index++) {
                    fprintf(stderr, "%02x", (unsigned int)buffer[index]);
                }
                fputc('\n', stderr);
                exit(1);
            }
            received += event.size;
            if (received == expected && !finished) finished = monotonic_nanos();
        } else if (event.type == KPB_EVENT_EXIT) {
            child_status = event.exit_status;
        } else if (event.type == KPB_EVENT_REPLAY_DONE && replay) {
            require(received == expected, "incomplete replay");
            if (!finished) finished = monotonic_nanos();
            require(kpb_send_input(&connection, "x", 1) == KPB_OK, "release producer");
        } else {
            require(event.type == KPB_EVENT_REPLAY_DONE, "unexpected live event");
        }
    }
    require(received == expected && finished >= started, "incomplete output");
    require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0, "producer exit");
    kpb_detach(&connection);
    do {
        waited = waitpid(status.broker_pid, &broker_status, 0);
    } while (waited < 0 && errno == EINTR);
    require(waited == status.broker_pid && WIFEXITED(broker_status), "broker exit");

    if (mode != BENCHMARK_PLAIN && !replay) {
        struct stat transcript;
        require(stat(transcript_path, &transcript) == 0, "stat transcript");
        if (mode == BENCHMARK_TRANSCRIPT_GRAPHICS) {
            require(
                transcript.st_size > 0 && (uint64_t)transcript.st_size < expected,
                "graphics transcript was not elided");
        } else {
            require(
                (uint64_t)transcript.st_size ==
                    expected + sizeof BENCHMARK_READY - 1U,
                "transcript size");
        }
        require(unlink(transcript_path) == 0, "remove transcript");
    }
    return finished - started;
}

static int
compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static size_t
parse_size(const char *text, size_t minimum, size_t maximum) {
    char *end = NULL;
    unsigned long long value;
    if (!text || !*text || text[0] == '-') return 0;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || !end || *end || value < minimum || value > maximum) return 0;
    return (size_t)value;
}

int
main(int argc, char **argv) {
    char program[KPB_PATH_MAX];
    char runtime_dir[] = "/tmp/kitty-pty-broker-benchmark.XXXXXX";
    size_t bytes = BENCHMARK_DEFAULT_BYTES;
    size_t samples = BENCHMARK_DEFAULT_SAMPLES;
    benchmark_mode mode;

    if (argc == 5 && strcmp(argv[1], "--producer") == 0) {
        size_t produce = parse_size(argv[2], 1, 1024ULL * 1024ULL * 1024ULL);
        if (!produce ||
            (strcmp(argv[3], "0") != 0 && strcmp(argv[3], "1") != 0) ||
            (strcmp(argv[4], "0") != 0 && strcmp(argv[4], "1") != 0)) {
            return 2;
        }
        return producer(
            produce, strcmp(argv[3], "1") == 0,
            strcmp(argv[4], "1") == 0);
    }
    if (argc == 3) {
        bytes = parse_size(argv[1], 4096, 1024ULL * 1024ULL * 1024ULL);
        samples = parse_size(argv[2], 1, BENCHMARK_SAMPLE_MAX);
        if (!bytes || !samples) {
            fprintf(stderr, "usage: %s [BYTES SAMPLES]\n", argv[0]);
            return 2;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [BYTES SAMPLES]\n", argv[0]);
        return 2;
    }
    require(realpath(argv[0], program) != NULL, "resolve benchmark path");
    require(mkdtemp(runtime_dir) != NULL, "create benchmark runtime");
    require(chmod(runtime_dir, 0700) == 0, "protect benchmark runtime");
    alarm(300);

    printf("bytes=%zu samples=%zu\n", bytes, samples);
    for (mode = BENCHMARK_PLAIN; mode <= BENCHMARK_REPLAY; mode++) {
        uint64_t timings[BENCHMARK_SAMPLE_MAX];
        size_t sample;
        uint64_t median;
        double seconds;
        for (sample = 0; sample < samples; sample++) {
            timings[sample] = run_sample(
                program, runtime_dir, mode, bytes, (unsigned int)sample);
        }
        qsort(timings, samples, sizeof timings[0], compare_u64);
        median = timings[samples / 2U];
        seconds = (double)median / 1000000000.0;
        printf(
            "%-31s %8.3f ns/byte  %7.3f GiB/s\n",
            mode_name(mode),
            (double)median / (double)(bytes +
                (mode == BENCHMARK_TRANSCRIPT_GRAPHICS ? 5U : 0U)),
            ((double)bytes / (1024.0 * 1024.0 * 1024.0)) / seconds);
    }
    alarm(0);
    {
        char sessions[KPB_PATH_MAX];
        require(
            snprintf(sessions, sizeof sessions, "%s/sessions", runtime_dir) > 0,
            "format sessions path");
        require(rmdir(sessions) == 0, "remove sessions directory");
    }
    require(rmdir(runtime_dir) == 0, "remove benchmark runtime");
    return 0;
}
