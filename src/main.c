#define _GNU_SOURCE

#include "kitty_pty_broker.h"
#include "tui.h"

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

/* `options` NULL keeps the plain version-1 attach that every existing caller
 * uses, which is also what makes this safe against a broker left running by a
 * previous build. */
static int
bridge(
    const char *runtime_dir,
    const char *session_id,
    const kpb_attach_options *options
) {
    unsigned char buffer[KPB_IO_CHUNK];
    struct termios saved;
    struct termios raw;
    struct sigaction action;
    struct winsize size;
    kpb_connection connection;
    kpb_attach_result attached;
    bool observing = options && options->mode == KPB_ATTACH_OBSERVE;
    bool have_termios = false;
    bool replay_done = false;
    bool track_cursor = false;
    uint64_t cursor_epoch = 0;
    uint64_t cursor_offset = 0;
    int exit_code = 0;
    kpb_result result;

    get_size(&size);
    memset(&attached, 0, sizeof attached);
    if (options) {
        kpb_attach_options request = *options;
        if (!observing) {
            request.rows = size.ws_row;
            request.columns = size.ws_col;
            request.xpixel = size.ws_xpixel;
            request.ypixel = size.ws_ypixel;
        }
        result = kpb_attach_with_options(
            runtime_dir, session_id, &request, &connection, &attached);
        if (result == KPB_OK && attached.version >= 2) {
            track_cursor = true;
            cursor_epoch = attached.journal_epoch;
            cursor_offset = attached.journal_offset;
        }
    } else {
        result = kpb_attach(
            runtime_dir, session_id,
            size.ws_row, size.ws_col, size.ws_xpixel, size.ws_ypixel,
            &connection
        );
    }
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
            /* An observer must never resize the pane it is watching. */
            if (!observing) {
                get_size(&size);
                (void)kpb_resize(
                    &connection, size.ws_row, size.ws_col, size.ws_xpixel, size.ws_ypixel
                );
            }
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
                cursor_offset += event.size;
                if (write_all(STDOUT_FILENO, buffer, event.size) != 0) {
                    exit_code = 1;
                    break;
                }
            } else if (event.type == KPB_EVENT_RESET) {
                /* Written but deliberately not counted: it holds no journal
                 * position, and adding it would push cursor_offset past the
                 * end of what has actually been received. */
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
            if (observing) {
                /* Read-only: local keys are consumed here, never forwarded.
                 * Ctrl-] leaves, matching the usual escape idiom. */
                if (received == 0) break;
                if (received > 0 && memchr(buffer, 0x1d, (size_t)received)) break;
                continue;
            }
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
    if (track_cursor) {
        /* Where to resume from next time.  Printed to stderr so it does not
         * contaminate the pane's own output. */
        fprintf(
            stderr, "kitty-pty-broker: cursor=%llu:%llu\n",
            (unsigned long long)cursor_epoch,
            (unsigned long long)cursor_offset);
    }
    return exit_code;
}

/* EPOCH:OFFSET, fully consumed, exactly one separator. */
static int
parse_cursor(const char *text, uint64_t *epoch, uint64_t *offset) {
    char *end = NULL;
    unsigned long long value;
    if (!text || !*text) return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || !end || *end != ':' || end == text) return -1;
    *epoch = (uint64_t)value;
    text = end + 1;
    if (!*text) return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || !end || *end != '\0') return -1;
    *offset = (uint64_t)value;
    return 0;
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
        "  kitty-pty-broker [--runtime-dir DIR] run [--id ID] [--journal-limit BYTES]\n"
        "                   [--transcript PATH] [--transcript-limit BYTES]\n"
        "                   [--transcript-graphics elide|keep] -- COMMAND [ARG...]\n"
        "  kitty-pty-broker [--runtime-dir DIR] attach ID [--resume EPOCH:OFFSET]\n"
        "  kitty-pty-broker [--runtime-dir DIR] observe ID [--from EPOCH:OFFSET]\n"
        "  kitty-pty-broker [--runtime-dir DIR] list [--json]\n"
        "  kitty-pty-broker [--runtime-dir DIR] status ID [--json]\n"
        "  kitty-pty-broker [--runtime-dir DIR] kill ID\n"
        "  kitty-pty-broker [--runtime-dir DIR] tui\n"
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
        printf(
            "%d.%d.%d protocol=%d protocol-max=%d\n",
            KPB_VERSION_MAJOR, KPB_VERSION_MINOR, KPB_VERSION_PATCH,
            kpb_protocol_version(), kpb_protocol_version_max());
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
            } else if (strcmp(argv[index], "--transcript") == 0 && index + 1 < argc) {
                if (argv[index + 1][0] != '/') {
                    fprintf(stderr, "kitty-pty-broker: transcript path must be absolute\n");
                    return 2;
                }
                options.transcript_path = argv[index + 1];
                index += 2;
            } else if (strcmp(argv[index], "--transcript-limit") == 0 && index + 1 < argc) {
                char *end = NULL;
                unsigned long long value = strtoull(argv[index + 1], &end, 10);
                if (!end || *end) {
                    fprintf(stderr, "kitty-pty-broker: invalid transcript limit\n");
                    return 2;
                }
                options.transcript_limit = value;
                index += 2;
            } else if (strcmp(argv[index], "--transcript-graphics") == 0 && index + 1 < argc) {
                if (strcmp(argv[index + 1], "elide") == 0) {
                    options.transcript_graphics = KPB_TRANSCRIPT_GRAPHICS_ELIDE;
                } else if (strcmp(argv[index + 1], "keep") == 0) {
                    options.transcript_graphics = KPB_TRANSCRIPT_GRAPHICS_KEEP;
                } else {
                    fprintf(stderr, "kitty-pty-broker: transcript graphics must be elide or keep\n");
                    return 2;
                }
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
        return bridge(runtime_dir, id, NULL);
    }
    if (strcmp(command, "observe") == 0) {
        kpb_attach_options options;
        kpb_attach_options_init(&options);
        options.mode = KPB_ATTACH_OBSERVE;
        if (index >= argc) {
            usage(stderr);
            return 2;
        }
        {
            const char *session_id = argv[index++];
            if (index < argc) {
                if (strcmp(argv[index], "--from") != 0 || index + 1 >= argc ||
                    parse_cursor(
                        argv[index + 1],
                        &options.resume_epoch, &options.resume_offset) != 0) {
                    usage(stderr);
                    return 2;
                }
                options.resume = 1;
                index += 2;
            }
            if (index != argc) {
                usage(stderr);
                return 2;
            }
            return bridge(runtime_dir, session_id, &options);
        }
    }
    if (strcmp(command, "attach") == 0) {
        const char *session_id;
        if (index >= argc) {
            usage(stderr);
            return 2;
        }
        session_id = argv[index++];
        /* Plain `attach ID` still emits a version-1 attach, so the shipped
         * path is unchanged and works against any broker. */
        if (index == argc) return bridge(runtime_dir, session_id, NULL);
        {
            kpb_attach_options options;
            kpb_attach_options_init(&options);
            if (strcmp(argv[index], "--resume") != 0 || index + 1 >= argc ||
                parse_cursor(
                    argv[index + 1],
                    &options.resume_epoch, &options.resume_offset) != 0 ||
                index + 2 != argc) {
                usage(stderr);
                return 2;
            }
            options.resume = 1;
            return bridge(runtime_dir, session_id, &options);
        }
    }
    if (strcmp(command, "tui") == 0) {
        char session_id[KPB_SESSION_ID_MAX + 1];
        int tui_result;
        if (index != argc) {
            usage(stderr);
            return 2;
        }
        tui_result = kpb_tui_run(runtime_dir, session_id);
        if (tui_result == KPB_TUI_ATTACH) {
            resize_pending = 0;
            stop_pending = 0;
            return bridge(runtime_dir, session_id, NULL);
        }
        return tui_result == KPB_TUI_QUIT ? 0 : 1;
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
