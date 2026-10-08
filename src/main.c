#define _GNU_SOURCE

#include "kitty_pty_broker.h"
#include "internal.h"
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

/* --timeout, in milliseconds; 0 selects each operation's own default (2 s,
 * and 1 s for list and tui). */
static int timeout_millis;

#define TIMEOUT_MIN_MILLIS 100
#define TIMEOUT_MAX_MILLIS 60000

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
parse_u64_prefix(const char *text, uint64_t *value, const char **end_out) {
    char *end = NULL;
    unsigned long long parsed;
    if (!text || text[0] < '0' || text[0] > '9') return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || !end || end == text || parsed > UINT64_MAX) return -1;
    *value = (uint64_t)parsed;
    *end_out = end;
    return 0;
}

static int
parse_storage_limit(const char *text, uint64_t *value) {
    const char *end;
    return parse_u64_prefix(text, value, &end) == 0 && *end == '\0' &&
        *value <= INT64_MAX ? 0 : -1;
}

static void report_socket_path_too_long(const char *runtime_dir, const char *session_id);

/* One reason line for every failure.  Printed to stderr, or - when a caller is
 * about to take over the screen (the TUI) - handed back in `failure` so it can
 * be shown where it will be seen. */
static void
say_failure(char *failure, size_t capacity, const char *verb, const char *session_id, const char *reason) {
    if (failure) {
        snprintf(failure, capacity, "%s %.64s: %s", verb, session_id, reason);
    } else {
        fprintf(stderr, "kitty-pty-broker: %s %s: %s\n", verb, session_id, reason);
    }
}

/* `options` NULL keeps the plain version-1 attach that every existing caller
 * uses, which is also what makes this safe against a broker left running by a
 * previous build.
 *
 * Every non-zero exit says why, on one line: a pane's own exit status is not a
 * failure of this function and is passed through silently, but "the peer closed",
 * "the broker stopped mid-frame" and the rest used to exit 1 with nothing on
 * stderr.  `failure`, if given, receives that line instead of stderr. */
static int
bridge(
    const char *runtime_dir,
    const char *session_id,
    const kpb_attach_options *options,
    char *failure,
    size_t failure_capacity
) {
    unsigned char buffer[KPB_IO_CHUNK];
    struct termios saved;
    struct termios raw;
    struct sigaction action;
    struct winsize size;
    sigset_t blocked;
    sigset_t original;
    kpb_connection connection;
    kpb_attach_result attached;
    bool observing = options && options->mode == KPB_ATTACH_OBSERVE;
    const char *verb = observing ? "observe" : "attach";
    char reason[200] = "";
    bool have_termios = false;
    bool replay_done = false;
    bool stdin_hung_up = false;
    bool stdin_hup_seen = false;
    bool track_cursor = false;
    uint64_t cursor_epoch = 0;
    uint64_t cursor_offset = 0;
    int exit_code = 0;
    kpb_result result;

    if (failure && failure_capacity) failure[0] = '\0';
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
        result = kpb_attach_with_options_timeout(
            runtime_dir, session_id, &request, &connection, &attached, timeout_millis);
        if (result == KPB_OK && attached.version >= 2) {
            track_cursor = true;
            cursor_epoch = attached.journal_epoch;
            cursor_offset = attached.journal_offset;
        }
    } else {
        result = kpb_attach_timeout(
            runtime_dir, session_id,
            size.ws_row, size.ws_col, size.ws_xpixel, size.ws_ypixel,
            &connection, timeout_millis
        );
    }
    if (result != KPB_OK) {
        if (result == KPB_ERR_NAME_TOO_LONG) {
            report_socket_path_too_long(runtime_dir, session_id);
            return 1;
        }
        if (result == KPB_ERR_SYSTEM) {
            snprintf(reason, sizeof reason, "%s: %s", kpb_result_string(result), strerror(errno));
        } else {
            snprintf(reason, sizeof reason, "%s", kpb_result_string(result));
        }
        say_failure(failure, failure_capacity, verb, session_id, reason);
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

    /* The pending flags are checked at the top of the loop, and the wait
     * below has to wake when one is set.  Handlers alone are not enough: a
     * signal landing between the check and the wait would be absorbed with
     * the wait still ahead, so the signals are blocked here and atomically
     * unblocked only inside ppoll - the same construction the broker uses
     * for SIGCHLD, rather than a fallback timeout that would wake every
     * attached client ten times a second for the life of the pane. */
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGWINCH);
    sigaddset(&blocked, SIGHUP);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    sigprocmask(SIG_BLOCK, &blocked, &original);

    while (!stop_pending) {
        struct pollfd descriptors[2];
        int count;
        bool stdin_watched;
        /* ppoll unblocks the signals only while it waits, and returns without
         * delivering one that is already pending if a descriptor is ready at
         * the same moment.  With a descriptor ready every time round - a
         * stream of output, or a hung-up stdin - a SIGTERM would then sit
         * pending forever.  Letting pending signals through here, between
         * waits, closes that. */
        sigprocmask(SIG_SETMASK, &original, NULL);
        sigprocmask(SIG_BLOCK, &blocked, NULL);
        if (stop_pending) break;
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
        /* A hung-up stdin (a pipe whose writer is gone) is reported by poll
         * whether or not it is being watched, and never clears: left in the set
         * it makes ppoll return at once, forever. */
        descriptors[1].fd = (stdin_hung_up || (stdin_hup_seen && !replay_done)) ? -1 : STDIN_FILENO;
        descriptors[1].events = replay_done ? POLLIN : 0;
        stdin_watched = replay_done;
        descriptors[1].revents = 0;
        count = ppoll(descriptors, 2, NULL, &original);
        if (count < 0) {
            if (errno == EINTR) continue;
            exit_code = 1;
            snprintf(reason, sizeof reason, "waiting for the broker failed: %s", strerror(errno));
            break;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            kpb_event event;
            result = kpb_receive(&connection, buffer, sizeof buffer, &event);
            if (result != KPB_OK) {
                if (descriptors[0].revents & POLLIN) {
                    exit_code = 1;
                    if (result == KPB_ERR_TIMEOUT) {
                        /* observe continues with --from, attach with --resume. */
                        snprintf(
                            reason, sizeof reason,
                            "the broker stopped in the middle of a frame (timed out); "
                            "reattach with %s %llu:%llu to continue",
                            observing ? "--from" : "--resume",
                            (unsigned long long)cursor_epoch,
                            (unsigned long long)cursor_offset);
                        if (!track_cursor) {
                            snprintf(
                                reason, sizeof reason,
                                "the broker stopped in the middle of a frame (timed out)");
                        }
                    } else if (result == KPB_ERR_PROTOCOL) {
                        snprintf(reason, sizeof reason, "protocol error from the broker");
                    } else if (result == KPB_ERR_BUFFER) {
                        snprintf(reason, sizeof reason, "a frame from the broker was too large");
                    } else {
                        snprintf(reason, sizeof reason, "connection closed by the broker without an exit status");
                    }
                }
                break;
            }
            if (event.type == KPB_EVENT_OUTPUT) {
                cursor_offset += event.size;
                if (write_all_fd(STDOUT_FILENO, buffer, event.size) < 0) {
                    exit_code = 1;
                    snprintf(reason, sizeof reason, "cannot write to the terminal: %s", strerror(errno));
                    break;
                }
            } else if (event.type == KPB_EVENT_RESET) {
                /* Written but deliberately not counted: it holds no journal
                 * position, and adding it would push cursor_offset past the
                 * end of what has actually been received. */
                if (write_all_fd(STDOUT_FILENO, buffer, event.size) < 0) {
                    exit_code = 1;
                    snprintf(reason, sizeof reason, "cannot write to the terminal: %s", strerror(errno));
                    break;
                }
            } else if (event.type == KPB_EVENT_REPLAY_DONE) {
                replay_done = true;
            } else if (event.type == KPB_EVENT_EXIT) {
                exit_code = wait_status_to_exit_code(event.exit_status);
                break;
            } else if (event.type == KPB_EVENT_ERROR) {
                size_t length = event.size < sizeof reason - 1 ? event.size : sizeof reason - 1;
                exit_code = 1;
                /* The broker's own words, with control bytes made harmless. */
                {
                    size_t index;
                    for (index = 0; index < length; index++) {
                        unsigned char c = buffer[index];
                        reason[index] = c >= 0x20 && c < 0x7f ? (char)c : '?';
                    }
                    reason[length] = '\0';
                }
                if (!length) snprintf(reason, sizeof reason, "refused by the broker");
                break;
            }
        }
        if (descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL) &&
            !(descriptors[1].revents & POLLIN)) {
            /* stdin has hung up, and what that means depends on whether it was
             * being watched for input when ppoll ran.  Poll masks POLLIN on a
             * descriptor that was not asked about it, so a pipe whose writer
             * already closed reports POLLHUP alone while data it was sent is
             * still unread.  Seen while NOT watched (the replay is not done),
             * that only means "stop polling it for now": the data is read once
             * the replay is done.  Only a hang-up seen while watched - POLLIN
             * was requested and there is nothing to read - is end of file. */
            if (stdin_watched) stdin_hung_up = true;
            else stdin_hup_seen = true;
        }
        if (stdin_hung_up) break;
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
                    snprintf(reason, sizeof reason, "lost the connection to the broker while sending input");
                    break;
                }
            } else if (received == 0) {
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                exit_code = 1;
                snprintf(reason, sizeof reason, "cannot read the terminal: %s", strerror(errno));
                break;
            }
        }
    }

    sigprocmask(SIG_SETMASK, &original, NULL);
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
    if (exit_code != 0 && reason[0]) say_failure(failure, failure_capacity, verb, session_id, reason);
    return exit_code;
}

/* EPOCH:OFFSET, fully consumed, exactly one separator. */
static int
parse_cursor(const char *text, uint64_t *epoch, uint64_t *offset) {
    const char *end;
    if (!epoch || !offset || parse_u64_prefix(text, epoch, &end) != 0 ||
        *end != ':') {
        return -1;
    }
    text = end + 1;
    return parse_u64_prefix(text, offset, &end) == 0 && *end == '\0' ? 0 : -1;
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

/* Every status object - `status --json` and each `list --json` item - carries
 * the session's own record (its fields keep their names, types and meaning)
 * followed by fields the caller reads client-side: cwd_now, boot_id and
 * start_ticks (null when unavailable).  `reachable` is added only under
 * `list --all`. */
typedef struct {
    bool reachable;
} json_shape;

static void
print_status_json(const kpb_status *status, json_shape shape) {
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
    {
        char now[KPB_PATH_MAX];
        int deleted = 0;
        fputs(",\"cwd_now\":", stdout);
        if (kpb_read_cwd_now_ex(status->child_pid, now, sizeof now, &deleted) == KPB_OK) {
            json_string(now);
        } else {
            fputs("null", stdout);
        }
        /* null because the directory was removed, as opposed to null because
         * it could not be read. */
        fputs(deleted ? ",\"cwd_now_deleted\":true" : ",\"cwd_now_deleted\":false", stdout);
    }
    fputs(",\"command\":", stdout);
    json_string(status->command);
    {
        char boot[64];
        uint64_t ticks;
        fputs(",\"boot_id\":", stdout);
        if (kpb_read_boot_id(boot) == KPB_OK) json_string(boot);
        else fputs("null", stdout);
        fputs(",\"start_ticks\":", stdout);
        if (kpb_read_start_ticks(status->broker_pid, &ticks) == KPB_OK) {
            printf("%llu", (unsigned long long)ticks);
        } else {
            fputs("null", stdout);
        }
    }
    if (shape.reachable) fputs(",\"reachable\":true", stdout);
    putchar('}');
}

/* A short, stable word for why a session could not be reached: what `list`
 * prints after the id and what `list --all --json` carries in "error". */
static const char *
reason_token(kpb_result result) {
    switch (result) {
        case KPB_ERR_TIMEOUT: return "timeout";
        case KPB_ERR_SECURITY: return "security";
        case KPB_ERR_PROTOCOL: return "protocol";
        case KPB_ERR_SYSTEM: return "system";
        case KPB_ERR_NAME_TOO_LONG: return "name-too-long";
        case KPB_ERR_INVALID: return "invalid";
        default: return "error";
    }
}

typedef struct {
    bool json;
    bool all;
    bool first;
} list_context;

static void
print_list_text(const kpb_status *status) {
    printf(
        "%s\t%s\tpid=%ld\t%s\n",
        status->session_id,
        status->attached ? "attached" : "detached",
        (long)status->child_pid,
        status->command
    );
}

static int
print_list_entry(const kpb_list_entry *entry, void *opaque) {
    list_context *context = opaque;
    if (!entry->reachable) {
        /* Always said on stderr, so a caller that only reads stdout and exits
         * 0 on success (the engine, the launcher) still gets the warning in its
         * logs; --all additionally puts the session in the listing. */
        fprintf(
            stderr, "kitty-pty-broker: list: %s: %s\n",
            entry->session_id, reason_token(entry->error));
        if (!context->all) return 0;
        if (context->json) {
            if (!context->first) putchar(',');
            fputs("{\"id\":", stdout);
            json_string(entry->session_id);
            fputs(",\"reachable\":false,\"error\":", stdout);
            json_string(reason_token(entry->error));
            fputs(",\"recorded\":{\"argv\":", stdout);
            fputs(entry->recorded.argv_json[0] ? entry->recorded.argv_json : "null", stdout);
            fputs(",\"cwd\":", stdout);
            fputs(entry->recorded.cwd_json[0] ? entry->recorded.cwd_json : "null", stdout);
            fputs(",\"started_millis\":", stdout);
            if (entry->recorded.have_started) printf("%llu", (unsigned long long)entry->recorded.started_millis);
            else fputs("null", stdout);
            printf(",\"truncated\":%s}", entry->recorded.truncated ? "true" : "false");
            putchar('}');
        } else {
            printf(
                "%s\tunreachable\tpid=-\terror=%s\n",
                entry->session_id, reason_token(entry->error));
        }
        context->first = false;
        return 0;
    }
    if (context->json) {
        json_shape shape = {.reachable = context->all};
        if (!context->first) putchar(',');
        print_status_json(&entry->status, shape);
    } else {
        print_list_text(&entry->status);
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
report_socket_path_too_long(const char *runtime_dir, const char *session_id) {
    char path[KPB_PATH_MAX + KPB_SESSION_ID_MAX + 32];
    (void)kpb_session_socket_path(runtime_dir, session_id, path, sizeof path);
    fprintf(
        stderr, "kitty-pty-broker: socket path too long (%zu bytes, limit %d): %s\n",
        strlen(path[0] ? path : runtime_dir), KPB_SOCKET_PATH_LIMIT,
        path[0] ? path : runtime_dir);
}

/* The failures every command that takes a runtime shares.  Checked up front so
 * a typo'd --runtime-dir says so, instead of looking like "no sessions" or
 * "session not found". */
static int
require_runtime(const char *runtime_dir) {
    kpb_result result = kpb_check_runtime(runtime_dir);
    if (result == KPB_OK) return 0;
    if (result == KPB_ERR_NOT_FOUND) {
        fprintf(
            stderr,
            "kitty-pty-broker: runtime directory does not exist: %s "
            "(set KITTY_PTY_BROKER_RUNTIME or pass --runtime-dir)\n",
            runtime_dir);
    } else if (result == KPB_ERR_SECURITY) {
        fprintf(
            stderr,
            "kitty-pty-broker: runtime directory is not safe to use (it must be a real "
            "directory owned by you, not a symlink): %s\n",
            runtime_dir);
    } else {
        fprintf(
            stderr, "kitty-pty-broker: runtime directory %s: %s\n",
            runtime_dir, kpb_result_string(result));
    }
    return 1;
}

/* Operations on one session: the same message for every command. */
static int
report_session_result(
    const char *operation,
    const char *runtime_dir,
    const char *session_id,
    kpb_result result
) {
    if (result == KPB_OK) return 0;
    if (result == KPB_ERR_NAME_TOO_LONG) {
        report_socket_path_too_long(runtime_dir, session_id);
        return 1;
    }
    return report_result(operation, result);
}

/* Decimal seconds, 0.1 to 60: digits with at most one point, nothing else.
 * strtod alone would also take leading space, signs, exponents, "inf" and
 * "nan", none of which a unit of seconds should quietly accept. */
static int
parse_timeout(const char *text, int *millis) {
    size_t index;
    size_t digits = 0;
    bool point = false;
    double seconds;
    char *end = NULL;
    if (!text || !*text) return -1;
    for (index = 0; text[index]; index++) {
        if (text[index] >= '0' && text[index] <= '9') {
            digits++;
        } else if (text[index] == '.' && !point) {
            point = true;
        } else {
            return -1;
        }
    }
    if (digits == 0 || digits > 12) return -1;
    seconds = strtod(text, &end);
    if (!end || *end != '\0') return -1;
    if (seconds < TIMEOUT_MIN_MILLIS / 1000.0 - 1e-9 || seconds > TIMEOUT_MAX_MILLIS / 1000.0 + 1e-9) {
        return -1;
    }
    *millis = (int)(seconds * 1000.0 + 0.5);
    if (*millis < TIMEOUT_MIN_MILLIS) *millis = TIMEOUT_MIN_MILLIS;
    if (*millis > TIMEOUT_MAX_MILLIS) *millis = TIMEOUT_MAX_MILLIS;
    return 0;
}

static void
usage(FILE *stream) {
    fputs(
        "kitty-pty-broker: keep a terminal pane alive without its frontend\n"
        "\n"
        "usage:\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] run [--id ID] [--journal-limit BYTES]\n"
        "                   [--transcript PATH] [--transcript-limit BYTES]\n"
        "                   [--transcript-graphics elide|keep] -- COMMAND [ARG...]\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] attach ID [--resume EPOCH:OFFSET]\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] observe ID [--from EPOCH:OFFSET]\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] list [--json] [--all]\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] status ID [--json]\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] kill ID [--expect-started MILLIS]\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] reaped [--json]\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] reaped path ID\n"
        "  kitty-pty-broker [GLOBAL OPTIONS] tui\n"
        "  kitty-pty-broker version\n"
        "  kitty-pty-broker help | -h | --help\n"
        "\n"
        "commands:\n"
        "  run       start a session running COMMAND and attach this terminal to it\n"
        "  attach    attach read-write (one client at a time); --resume continues from a cursor\n"
        "  observe   attach read-only; keys are not forwarded, Ctrl-] leaves\n"
        "  list      one line per reachable session; --json for a JSON array;\n"
        "            --all also lists sessions that did not answer, as unreachable\n"
        "            JSON unreachable rows include recorded spawn argv/cwd (null if unavailable)\n"
        "  status    one session's record; JSON also has cwd_now (the command's directory now;\n"
        "            cwd is where it started), boot_id and start_ticks (null if unavailable)\n"
        "  kill      ask a session to end (SIGTERM, then SIGKILL after a grace period).\n"
        "            With --expect-started MILLIS (a status's started_millis) the broker ends\n"
        "            only the session that started then, atomically: exit 3 if the id now\n"
        "            names a different session, 5 if the broker is too old to check; either\n"
        "            way nothing was done\n"
        "  reaped    journals kept from sessions whose broker died; `reaped path ID` prints\n"
        "            the newest journal for ID\n"
        "  tui       interactive session manager (Enter attach, o observe, x end, q quit)\n"
        "\n"
        "global options (before the command, in either order):\n"
        "  --runtime-dir DIR   the runtime directory, an absolute path. Default: the absolute\n"
        "                      $KITTY_PTY_BROKER_RUNTIME if set, else\n"
        "                      $XDG_RUNTIME_DIR/kitty-pty-broker, else\n"
        "                      /tmp/kitty-pty-broker-UID\n"
        "  --timeout SECONDS   bound on each wait for a broker, in decimal SECONDS from 0.1 to\n"
        "                      60 (default 2; list and tui default to 1). A session that does\n"
        "                      not answer in time is unreachable; list asks all sessions at\n"
        "                      once, so the bound is shared, not per session\n"
        "\n"
        "exit status: 0 success; 1 failure (including a missing or unsafe runtime directory);\n"
        "2 usage error; 3 and 5 are refusals of kill --expect-started (above); 6 is a kill whose\n"
        "request was never sent (timed out while connecting: nothing was done). run and attach\n"
        "exit with the pane command's status once it exits; a failure to attach or stay\n"
        "attached exits 1 and says why on one line.\n",
        stream
    );
}

static bool
is_help_flag(const char *argument) {
    return strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0;
}

typedef struct {
    bool json;
} reaped_context;

static int
print_reaped_entry(const kpb_reaped_entry *entry, void *opaque) {
    static bool first = true;
    reaped_context *context = opaque;
    if (context->json) {
        if (!first) putchar(',');
        fputs("{\"id\":", stdout);
        json_string(entry->session_id);
        printf(
            ",\"started_millis\":%llu,\"reaped_millis\":",
            (unsigned long long)entry->started_millis);
        if (entry->reaped_millis) printf("%llu", (unsigned long long)entry->reaped_millis);
        else fputs("null", stdout);
        printf(",\"journal_bytes\":%llu,\"journal\":", (unsigned long long)entry->journal_bytes);
        json_string(entry->journal_path);
        fputs(",\"meta\":", stdout);
        if (entry->meta_path[0]) json_string(entry->meta_path);
        else fputs("null", stdout);
        putchar('}');
    } else {
        printf(
            "%s\tstarted=%llu\tbytes=%llu\t%s\n",
            entry->session_id,
            (unsigned long long)entry->started_millis,
            (unsigned long long)entry->journal_bytes,
            entry->journal_path);
    }
    first = false;
    return 0;
}

/* Parse the global options that precede the command.  Returns 0 to continue,
 * or an exit code (help is 0 and `done` is set). */
static int
parse_global_options(
    int argc,
    char **argv,
    int *index,
    const char **runtime_dir,
    bool *done
) {
    *done = false;
    while (*index < argc) {
        const char *argument = argv[*index];
        if (is_help_flag(argument)) {
            usage(stdout);
            *done = true;
            return 0;
        }
        if (strcmp(argument, "--runtime-dir") == 0) {
            if (*index + 1 >= argc) {
                usage(stderr);
                *done = true;
                return 2;
            }
            *runtime_dir = argv[*index + 1];
            *index += 2;
        } else if (strcmp(argument, "--timeout") == 0) {
            if (*index + 1 >= argc || parse_timeout(argv[*index + 1], &timeout_millis) != 0) {
                fprintf(
                    stderr,
                    "kitty-pty-broker: --timeout takes SECONDS, a decimal from 0.1 to 60\n");
                *done = true;
                return 2;
            }
            *index += 2;
        } else {
            break;
        }
    }
    return 0;
}

int
main(int argc, char **argv) {
    char runtime_buffer[KPB_PATH_MAX];
    const char *runtime_dir = NULL;
    const char *command;
    int index = 1;
    bool done;
    int code = parse_global_options(argc, argv, &index, &runtime_dir, &done);
    if (done) return code;
    if (!runtime_dir) runtime_dir = default_runtime(runtime_buffer);
    if (!runtime_dir || index >= argc) {
        usage(stderr);
        return 2;
    }
    command = argv[index++];
    if (strcmp(command, "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(command, "version") == 0) {
        printf(
            "%d.%d.%d protocol=%d protocol-max=%d\n",
            KPB_VERSION_MAJOR, KPB_VERSION_MINOR, KPB_VERSION_PATCH,
            kpb_protocol_version(), kpb_protocol_version_max());
        return 0;
    }
    /* `COMMAND --help` is help, not a usage error.  Arguments after `--` belong
     * to the command being run and are never inspected. */
    {
        int scan;
        for (scan = index; scan < argc && strcmp(argv[scan], "--") != 0; scan++) {
            if (is_help_flag(argv[scan])) {
                usage(stdout);
                return 0;
            }
        }
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
                uint64_t value;
                if (parse_storage_limit(argv[index + 1], &value) != 0) {
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
                uint64_t value;
                if (parse_storage_limit(argv[index + 1], &value) != 0) {
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
        result = kpb_spawn_timeout(&options, &status, timeout_millis);
        if (result == KPB_ERR_TIMEOUT) {
            fprintf(
                stderr,
                "kitty-pty-broker: start session: timed out: the sessions lock stayed busy, "
                "or the new broker did not answer in time; if it is only slow it removes the "
                "session itself\n");
            return 1;
        }
        if (result != KPB_OK) return report_session_result("start session", runtime_dir, id, result);
        return bridge(runtime_dir, id, NULL, NULL, 0);
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
            if (require_runtime(runtime_dir) != 0) return 1;
            return bridge(runtime_dir, session_id, &options, NULL, 0);
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
        if (index == argc) {
            if (require_runtime(runtime_dir) != 0) return 1;
            return bridge(runtime_dir, session_id, NULL, NULL, 0);
        }
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
            if (require_runtime(runtime_dir) != 0) return 1;
            return bridge(runtime_dir, session_id, &options, NULL, 0);
        }
    }
    if (strcmp(command, "tui") == 0) {
        char session_id[KPB_SESSION_ID_MAX + 1];
        int tui_result;
        if (index != argc) {
            usage(stderr);
            return 2;
        }
        if (require_runtime(runtime_dir) != 0) return 1;
        char carried[256] = "";
        for (;;) {
            tui_result = kpb_tui_run(runtime_dir, timeout_millis, session_id, carried);
            carried[0] = '\0';
            if (tui_result == KPB_TUI_ATTACH) {
                resize_pending = 0;
                stop_pending = 0;
                return bridge(runtime_dir, session_id, NULL, NULL, 0);
            }
            if (tui_result == KPB_TUI_OBSERVE) {
                kpb_attach_options options;
                kpb_attach_options_init(&options);
                options.mode = KPB_ATTACH_OBSERVE;
                resize_pending = 0;
                stop_pending = 0;
                /* Watched on the alternate screen so leaving it with Ctrl-]
                 * puts the list back exactly as it was, with nothing from the
                 * pane left in the scrollback. */
                fputs("\033[?1049h\033[2J\033[H", stdout);
                fflush(stdout);
                /* A failure is not printed here: it would be written inside the
                 * alternate screen and gone the moment it is left.  It is
                 * carried to the list instead, where it stays on screen. */
                (void)bridge(runtime_dir, session_id, &options, carried, sizeof carried);
                fputs("\033[?1049l", stdout);
                fflush(stdout);
                continue;
            }
            return tui_result == KPB_TUI_QUIT ? 0 : 1;
        }
    }
    if (strcmp(command, "kill") == 0) {
        kpb_result result;
        const char *session_id;
        bool bound = false;
        uint64_t expected = 0;
        if (index >= argc) {
            usage(stderr);
            return 2;
        }
        session_id = argv[index++];
        if (index < argc) {
            const char *end;
            if (strcmp(argv[index], "--expect-started") != 0 || index + 2 != argc ||
                parse_u64_prefix(argv[index + 1], &expected, &end) != 0 || *end != '\0') {
                usage(stderr);
                return 2;
            }
            bound = true;
        }
        if (require_runtime(runtime_dir) != 0) return 1;
        if (bound) {
            result = kpb_terminate_expect(runtime_dir, session_id, expected, timeout_millis);
            if (result == KPB_ERR_MISMATCH) {
                fprintf(
                    stderr,
                    "kitty-pty-broker: kill session: refused: %s is not the session that "
                    "started at %llu (it was replaced); nothing was done\n",
                    session_id, (unsigned long long)expected);
                return 3;
            }
            if (result == KPB_ERR_UNSUPPORTED) {
                fprintf(
                    stderr,
                    "kitty-pty-broker: kill session: this broker predates identity-checked "
                    "kill and cannot verify %s; nothing was done\n", session_id);
                return 5;
            }
        } else {
            result = kpb_terminate_timeout(runtime_dir, session_id, timeout_millis);
        }
        if (result == KPB_ERR_NOT_SENT) {
            /* Still connecting when the time ran out: nothing was sent, so
             * nothing was or will be done and it is safe to try again. */
            fprintf(
                stderr,
                "kitty-pty-broker: kill session: timed out before the request was sent "
                "(the broker is not accepting connections); nothing was done\n");
            return 6;
        }
        if (result == KPB_ERR_TIMEOUT) {
            fprintf(
                stderr,
                "kitty-pty-broker: kill session: timed out; "
                "the broker may still act on the request\n");
            return 1;
        }
        return report_session_result("kill session", runtime_dir, session_id, result);
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
        if (require_runtime(runtime_dir) != 0) return 1;
        result = kpb_query_status_timeout(runtime_dir, session_id, &status, timeout_millis);
        if (result != KPB_OK) {
            return report_session_result("query session", runtime_dir, session_id, result);
        }
        if (json) {
            json_shape shape = {.reachable = false};
            print_status_json(&status, shape);
            putchar('\n');
        } else {
            print_list_text(&status);
        }
        return 0;
    }
    if (strcmp(command, "list") == 0) {
        list_context context = {.json = false, .all = false, .first = true};
        kpb_list_options options;
        kpb_result result;
        while (index < argc) {
            if (strcmp(argv[index], "--json") == 0) {
                context.json = true;
            } else if (strcmp(argv[index], "--all") == 0) {
                context.all = true;
            } else {
                usage(stderr);
                return 2;
            }
            index++;
        }
        if (require_runtime(runtime_dir) != 0) return 1;
        options.timeout_millis = timeout_millis;
        if (context.json) putchar('[');
        result = kpb_list_with_options(runtime_dir, &options, print_list_entry, &context);
        if (context.json) puts("]");
        return report_result("list sessions", result);
    }
    if (strcmp(command, "reaped") == 0) {
        reaped_context context = {.json = false};
        kpb_result result;
        if (index < argc && strcmp(argv[index], "path") == 0) {
            char path[KPB_PATH_MAX];
            if (index + 2 != argc) {
                usage(stderr);
                return 2;
            }
            if (require_runtime(runtime_dir) != 0) return 1;
            result = kpb_reaped_path(runtime_dir, argv[index + 1], path, sizeof path);
            if (result == KPB_ERR_NOT_FOUND) {
                fprintf(
                    stderr, "kitty-pty-broker: reaped: no archived journal for %s\n",
                    argv[index + 1]);
                return 1;
            }
            if (result != KPB_OK) return report_result("reaped path", result);
            puts(path);
            return 0;
        }
        if (index < argc && strcmp(argv[index], "--json") == 0) {
            context.json = true;
            index++;
        }
        if (index != argc) {
            usage(stderr);
            return 2;
        }
        if (require_runtime(runtime_dir) != 0) return 1;
        if (context.json) putchar('[');
        result = kpb_list_reaped(runtime_dir, print_reaped_entry, &context);
        if (context.json) puts("]");
        return report_result("list reaped journals", result);
    }
    usage(stderr);
    return 2;
}
