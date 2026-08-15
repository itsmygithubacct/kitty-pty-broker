#ifndef KITTY_PTY_BROKER_INTERNAL_H
#define KITTY_PTY_BROKER_INTERNAL_H

/* Process-level helpers shared by the library, the CLI, the TUI, and the
 * test tools.  One definition each, for the same reason deadline_in gives
 * for the timespec carry: logic like the EINTR/short-transfer loop drifts
 * when every binary keeps its own copy - and the copies had in fact already
 * drifted before they were gathered here. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static inline ssize_t
write_all_fd(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, cursor + written, size - written);
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

static inline ssize_t
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

static inline int
wait_status_to_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
}

static inline uint64_t
clock_millis(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static inline uint64_t
realtime_millis(void) {
    return clock_millis(CLOCK_REALTIME);
}

static inline uint64_t
monotonic_millis(void) {
    return clock_millis(CLOCK_MONOTONIC);
}

#endif
