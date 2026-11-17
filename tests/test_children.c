/* signal_owned_children on kernels with and without the per-task children
 * file.  Kernels built without CONFIG_PROC_CHILDREN have no
 * /proc/<pid>/task/<tid>/children; the broker then finds its children by
 * scanning /proc.  Every scenario runs twice: on this kernel's own path, and
 * with the scan forced, so both paths are proved wherever the suite runs. */
#define _GNU_SOURCE
#include "kitty_pty_broker.c"

#include <stdio.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #value, \
            force_child_scan ? "forced scan" : "kernel path"); exit(1); \
} } while (0)

static pid_t
sleeping_child(void) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        for (;;) pause();
    }
    return pid;
}

/* A child that starts a grandchild, reports its PID and sleeps.  The
 * grandchild is not ours, so it must never be signalled. */
static pid_t
child_with_grandchild(pid_t *grandchild) {
    int report[2];
    pid_t pid;
    CHECK(pipe(report) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        pid_t inner = fork();
        if (inner < 0) _exit(2);
        if (inner == 0) {
            close(report[0]);
            close(report[1]);
            for (;;) pause();
        }
        if (write(report[1], &inner, sizeof inner) != (ssize_t)sizeof inner) _exit(3);
        close(report[1]);
        for (;;) pause();
    }
    close(report[1]);
    CHECK(read(report[0], grandchild, sizeof *grandchild) == (ssize_t)sizeof *grandchild);
    close(report[0]);
    return pid;
}

static void
expect_terminated(pid_t pid) {
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
}

static void
scenario(bool forced) {
    pid_t sleepers[3], zombie, parent, grandchild;
    int status, index;
    force_child_scan = forced;

    for (index = 0; index < 3; index++) sleepers[index] = sleeping_child();
    zombie = fork();
    CHECK(zombie >= 0);
    if (zombie == 0) _exit(7);
    parent = child_with_grandchild(&grandchild);
    /* Let the zombie finish exiting so the list really holds a zombie. */
    for (index = 0; index < 500; index++) {
        if (waitid(P_PID, (id_t)zombie, &(siginfo_t){0}, WEXITED | WNOHANG | WNOWAIT) == 0 &&
            kill(zombie, 0) == 0) {
            char path[64], buffer[256];
            int fd;
            ssize_t length;
            snprintf(path, sizeof path, "/proc/%ld/stat", (long)zombie);
            fd = open(path, O_RDONLY);
            if (fd >= 0) {
                length = read(fd, buffer, sizeof buffer - 1);
                close(fd);
                if (length > 0) {
                    buffer[length] = '\0';
                    if (strstr(buffer, ") Z ")) break;
                }
            }
        }
        usleep(2000);
    }

    CHECK(signal_owned_children(SIGTERM, monotonic_millis() + 5000) == 0);

    for (index = 0; index < 3; index++) expect_terminated(sleepers[index]);
    expect_terminated(parent);
    CHECK(waitpid(zombie, &status, 0) == zombie);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    /* The grandchild was never ours: still alive after its parent died. */
    usleep(50000);
    CHECK(kill(grandchild, 0) == 0);
    CHECK(kill(grandchild, SIGKILL) == 0);

    /* No children left: both paths report success and signal nothing. */
    CHECK(signal_owned_children(SIGTERM, monotonic_millis() + 5000) == 0);
}

static void
parent_field_survives_awkward_names(void) {
    /* A command name with spaces and ')' must not shift the parent field. */
    long parent = 0;
    CHECK(prctl(PR_SET_NAME, "a) b ) c", 0, 0, 0) == 0);
    CHECK(read_parent_pid((long)getpid(), &parent) == 0);
    CHECK(parent == (long)getppid());
    CHECK(read_parent_pid(0, &parent) != 0);
}

int
main(void) {
    char path[96];
    snprintf(path, sizeof path, "/proc/self/task/%ld/children", (long)getpid());
    printf("children file %s on this kernel\n", access(path, R_OK) == 0 ? "present" : "absent");
    scenario(false);
    scenario(true);
    parent_field_survives_awkward_names();
    puts("children tests passed");
    return 0;
}
