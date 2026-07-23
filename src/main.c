#define _GNU_SOURCE

#include "kitty_pty_broker.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t resize_pending;
static volatile sig_atomic_t stop_pending;

static void
handle_resize(int signal_number) {
    (void)signal_number;
    resize_pending = 1;
}

static void
handle_stop(int signal_number) {
    (void)signal_number;
    stop_pending = 1;
}

static int
write_all(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    size_t done = 0;
    while (done < size) {
        ssize_t count = write(fd, cursor + done, size - done);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) return -1;
        done += (size_t)count;
    }
    return 0;
}

static const char *
default_runtime(char output[KPB_PATH_MAX]) {
    const char *configured = getenv("KITTY_PTY_BROKER_RUNTIME");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    int count;
    if (configured && configured[0] == '/') return configured;
    if (xdg && xdg[0] == '/') {
        count = snprintf(output, KPB_PATH_MAX, "%s/kitty-pty-broker", xdg);
    } else {
        count = snprintf(output, KPB_PATH_MAX, "/tmp/kitty-pty-broker-%lu", (unsigned long)geteuid());
    }
    return count < 0 || count >= KPB_PATH_MAX ? NULL : output;
}

static void
get_size(struct winsize *size) {
    memset(size, 0, sizeof *size);
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, size) != 0) {
        size->ws_row = 24;
        size->ws_col = 80;
    }
}

static int
exit_code_from_wait_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
}

static int
bridge(const char *runtime_dir, const char *session_id) {
    unsigned char buffer[KPB_IO_CHUNK];
    struct termios saved;
    struct termios raw;
    struct sigaction action;
    struct winsize size;
    kpb_connection connection;
    bool have_termios = false;
    bool replay_done = false;
    int exit_code = 0;
    kpb_result result;

    get_size(&size);
    result = kpb_attach(
        runtime_dir, session_id,
        size.ws_row, size.ws_col, size.ws_xpixel, size.ws_ypixel,
        &connection
    );
    if (result != KPB_OK) {
        fprintf(stderr, "kitty-pty-broker: attach %s: %s", session_id, kpb_result_string(result));
        if (result == KPB_ERR_SYSTEM) fprintf(stderr, ": %s", strerror(errno));
        fputc('\n', stderr);
        return 1;
    }

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) have_termios = true;
    }

    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = handle_resize;
    sigaction(SIGWINCH, &action, NULL);
    action.sa_handler = handle_stop;
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    while (!stop_pending) {
        struct pollfd descriptors[2];
        int count;
        if (resize_pending) {
            resize_pending = 0;
            get_size(&size);
            (void)kpb_resize(
                &connection, size.ws_row, size.ws_col, size.ws_xpixel, size.ws_ypixel
            );
        }
        descriptors[0].fd = connection.fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = replay_done ? POLLIN : 0;
        descriptors[1].revents = 0;
        count = poll(descriptors, 2, 100);
        if (count < 0) {
            if (errno == EINTR) continue;
            exit_code = 1;
            break;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            kpb_event event;
            result = kpb_receive(&connection, buffer, sizeof buffer, &event);
            if (result != KPB_OK) {
                if (descriptors[0].revents & POLLIN) exit_code = 1;
                break;
            }
            if (event.type == KPB_EVENT_OUTPUT) {
                if (write_all(STDOUT_FILENO, buffer, event.size) != 0) {
                    exit_code = 1;
                    break;
                }
            } else if (event.type == KPB_EVENT_REPLAY_DONE) {
                replay_done = true;
            } else if (event.type == KPB_EVENT_EXIT) {
                exit_code = exit_code_from_wait_status(event.exit_status);
                break;
            } else if (event.type == KPB_EVENT_ERROR) {
                if (event.size) {
                    (void)write_all(STDERR_FILENO, buffer, event.size);
                    (void)write_all(STDERR_FILENO, "\n", 1);
                }
                exit_code = 1;
                break;
            }
        }
        if (descriptors[1].revents & POLLIN) {
            ssize_t received = read(STDIN_FILENO, buffer, sizeof buffer);
            if (received > 0) {
                result = kpb_send_input(&connection, buffer, (size_t)received);
                if (result != KPB_OK) {
                    exit_code = 1;
                    break;
                }
            } else if (received == 0) {
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                exit_code = 1;
                break;
            }
        }
    }

    kpb_detach(&connection);
    if (have_termios) (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return exit_code;
}

static void
json_string(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    putchar('"');
    while (*cursor) {
        switch (*cursor) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*cursor < 0x20) printf("\\u%04x", *cursor);
                else putchar(*cursor);
        }
        cursor++;
    }
    putchar('"');
}

static void
print_status_json(const kpb_status *status) {
    fputs("{\"id\":", stdout);
    json_string(status->session_id);
    printf(
        ",\"broker_pid\":%ld,\"child_pid\":%ld,\"foreground_pgrp\":%ld"
        ",\"started_millis\":%llu,\"journal_bytes\":%llu,\"journal_epoch\":%llu"
        ",\"attached\":%s,\"replay_complete\":%s,\"rows\":%u,\"columns\":%u"
        ",\"cwd\":",
        (long)status->broker_pid,
        (long)status->child_pid,
        (long)status->foreground_pgrp,
        (unsigned long long)status->started_millis,
        (unsigned long long)status->journal_bytes,
        (unsigned long long)status->journal_epoch,
        status->attached ? "true" : "false",
        status->replay_complete ? "true" : "false",
        status->rows,
        status->columns
    );
    json_string(status->cwd);
    fputs(",\"command\":", stdout);
    json_string(status->command);
    putchar('}');
}

typedef struct {
    bool json;
    bool first;
} list_context;

static int
print_list_item(const kpb_status *status, void *opaque) {
    list_context *context = opaque;
    if (context->json) {
        if (!context->first) putchar(',');
        print_status_json(status);
    } else {
        printf(
            "%s\t%s\tpid=%ld\t%s\n",
            status->session_id,
            status->attached ? "attached" : "detached",
            (long)status->child_pid,
            status->command
        );
    }
    context->first = false;
    return 0;
}

static int
report_result(const char *operation, kpb_result result) {
    if (result == KPB_OK) return 0;
    fprintf(stderr, "kitty-pty-broker: %s: %s", operation, kpb_result_string(result));
    if (result == KPB_ERR_SYSTEM || result == KPB_ERR_CHILD) {
        fprintf(stderr, ": %s", strerror(errno));
    }
    fputc('\n', stderr);
    return 1;
}

static void
usage(FILE *stream) {
    fputs(
        "usage:\n"
        "  kitty-pty-broker [--runtime-dir DIR] run [--id ID] [--journal-limit BYTES] -- COMMAND [ARG...]\n"
        "  kitty-pty-broker [--runtime-dir DIR] attach ID\n"
        "  kitty-pty-broker [--runtime-dir DIR] list [--json]\n"
        "  kitty-pty-broker [--runtime-dir DIR] status ID [--json]\n"
        "  kitty-pty-broker [--runtime-dir DIR] kill ID\n"
        "  kitty-pty-broker version\n",
        stream
    );
}

int
main(int argc, char **argv) {
    char runtime_buffer[KPB_PATH_MAX];
    const char *runtime_dir = NULL;
    const char *command;
    int index = 1;
    if (index < argc && strcmp(argv[index], "--runtime-dir") == 0) {
        if (index + 1 >= argc) {
            usage(stderr);
            return 2;
        }
        runtime_dir = argv[index + 1];
        index += 2;
    }
    if (!runtime_dir) runtime_dir = default_runtime(runtime_buffer);
    if (!runtime_dir || index >= argc) {
        usage(stderr);
        return 2;
    }
    command = argv[index++];
    if (strcmp(command, "version") == 0) {
        printf("%d.%d.%d protocol=%d\n", KPB_VERSION_MAJOR, KPB_VERSION_MINOR, KPB_VERSION_PATCH, kpb_protocol_version());
        return 0;
    }
    if (strcmp(command, "run") == 0) {
        kpb_spawn_options options;
        kpb_status status;
        struct winsize size;
        char generated[KPB_SESSION_ID_MAX + 1];
        const char *id = NULL;
        const char *cwd;
        kpb_result result;
        kpb_spawn_options_init(&options);
        while (index < argc && strcmp(argv[index], "--") != 0) {
            if (strcmp(argv[index], "--id") == 0 && index + 1 < argc) {
                id = argv[index + 1];
                index += 2;
            } else if (strcmp(argv[index], "--journal-limit") == 0 && index + 1 < argc) {
                char *end = NULL;
                unsigned long long value = strtoull(argv[index + 1], &end, 10);
                if (!end || *end) {
                    fprintf(stderr, "kitty-pty-broker: invalid journal limit\n");
                    return 2;
                }
                options.journal_limit = value;
                index += 2;
            } else {
                usage(stderr);
                return 2;
            }
        }
        if (index >= argc || strcmp(argv[index], "--") != 0 || index + 1 >= argc) {
            usage(stderr);
            return 2;
        }
        index++;
        if (!id) {
            result = kpb_generate_session_id(generated);
            if (result != KPB_OK) return report_result("generate session id", result);
            id = generated;
        }
        cwd = getcwd(runtime_buffer, sizeof runtime_buffer);
        if (!cwd) return report_result("get current directory", KPB_ERR_SYSTEM);
        get_size(&size);
        options.runtime_dir = runtime_dir;
        options.session_id = id;
        options.cwd = cwd;
        options.argv = &argv[index];
        options.rows = size.ws_row;
        options.columns = size.ws_col;
        options.xpixel = size.ws_xpixel;
        options.ypixel = size.ws_ypixel;
        result = kpb_spawn(&options, &status);
        if (result != KPB_OK) return report_result("start session", result);
        return bridge(runtime_dir, id);
    }
    if (strcmp(command, "attach") == 0) {
        if (index + 1 != argc) {
            usage(stderr);
            return 2;
        }
        return bridge(runtime_dir, argv[index]);
    }
    if (strcmp(command, "kill") == 0) {
        kpb_result result;
        if (index + 1 != argc) {
            usage(stderr);
            return 2;
        }
        result = kpb_terminate(runtime_dir, argv[index]);
        return report_result("kill session", result);
    }
    if (strcmp(command, "status") == 0) {
        kpb_status status;
        bool json = false;
        const char *session_id;
        kpb_result result;
        if (index >= argc) {
            usage(stderr);
            return 2;
        }
        session_id = argv[index++];
        if (index < argc && strcmp(argv[index], "--json") == 0) {
            json = true;
            index++;
        }
        if (index != argc) {
            usage(stderr);
            return 2;
        }
        result = kpb_query_status(runtime_dir, session_id, &status);
        if (result != KPB_OK) return report_result("query session", result);
        if (json) {
            print_status_json(&status);
            putchar('\n');
        } else {
            list_context context = {.json = false, .first = true};
            print_list_item(&status, &context);
        }
        return 0;
    }
    if (strcmp(command, "list") == 0) {
        list_context context = {.json = false, .first = true};
        kpb_result result;
        if (index < argc && strcmp(argv[index], "--json") == 0) {
            context.json = true;
            index++;
        }
        if (index != argc) {
            usage(stderr);
            return 2;
        }
        if (context.json) putchar('[');
        result = kpb_list(runtime_dir, print_list_item, &context);
        if (context.json) puts("]");
        return report_result("list sessions", result);
    }
    usage(stderr);
    return 2;
}
