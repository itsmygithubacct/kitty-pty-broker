#define _POSIX_C_SOURCE 200809L

#include "tui.h"

#include "internal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TUI_SESSION_LIMIT 2048U
#define TUI_MESSAGE_MAX 256U
#define TUI_DEFAULT_TIMEOUT_MILLIS 1000

#define TUI_RESET "\033[0m"
#define TUI_TITLE "\033[1;37m"
#define TUI_ACCENT "\033[34m"
#define TUI_ALERT "\033[31m"
#define TUI_MUTED "\033[2;37m"
#define TUI_SELECTED "\033[1;37;44m"
#define TUI_DANGER "\033[1;37;41m"

/* Sessions that did not answer are kept, shown as `unreachable`, so a wedged
 * broker is visible instead of silently absent.  Each item carries the whole
 * list entry: `status` is meaningful only while `reachable` is set. */
typedef struct {
    kpb_list_entry *items;
    size_t count;
    size_t capacity;
    bool truncated;
    bool allocation_failed;
} session_list;

typedef struct {
    struct termios saved_termios;
    struct sigaction saved_winch;
    struct sigaction saved_hup;
    struct sigaction saved_term;
    struct sigaction saved_int;
    bool have_termios;
    bool have_winch;
    bool have_hup;
    bool have_term;
    bool have_int;
    bool alternate_screen;
} terminal_state;

static volatile sig_atomic_t tui_resize_pending;
static volatile sig_atomic_t tui_stop_pending;

static void
handle_tui_resize(int signal_number) {
    (void)signal_number;
    tui_resize_pending = 1;
}

static void
handle_tui_stop(int signal_number) {
    (void)signal_number;
    tui_stop_pending = 1;
}

static int
collect_session(const kpb_list_entry *entry, void *opaque) {
    session_list *list = opaque;
    kpb_list_entry *resized;
    size_t capacity;
    if (list->count >= TUI_SESSION_LIMIT) {
        list->truncated = true;
        return 1;
    }
    if (list->count == list->capacity) {
        capacity = list->capacity ? list->capacity * 2U : 16U;
        if (capacity > TUI_SESSION_LIMIT) capacity = TUI_SESSION_LIMIT;
        resized = realloc(list->items, capacity * sizeof *resized);
        if (!resized) {
            list->allocation_failed = true;
            return 1;
        }
        list->items = resized;
        list->capacity = capacity;
    }
    list->items[list->count++] = *entry;
    return 0;
}

static int
compare_sessions(const void *left_opaque, const void *right_opaque) {
    const kpb_list_entry *left = left_opaque;
    const kpb_list_entry *right = right_opaque;
    if (!!left->reachable != !!right->reachable) {
        return left->reachable ? -1 : 1;
    }
    if (left->reachable) {
        if (!!left->status.attached != !!right->status.attached) {
            return left->status.attached ? 1 : -1;
        }
        if (left->status.started_millis < right->status.started_millis) return 1;
        if (left->status.started_millis > right->status.started_millis) return -1;
    }
    return strcmp(left->session_id, right->session_id);
}

static kpb_result
refresh_sessions(
    const char *runtime_dir,
    int timeout_millis,
    session_list *list,
    size_t *selected
) {
    kpb_list_options options;
    char selected_id[KPB_SESSION_ID_MAX + 1] = "";
    size_t index;
    kpb_result result;
    if (list->count && *selected < list->count) {
        memcpy(
            selected_id,
            list->items[*selected].session_id,
            sizeof selected_id
        );
        selected_id[sizeof selected_id - 1U] = '\0';
    }
    list->count = 0;
    list->truncated = false;
    list->allocation_failed = false;
    options.timeout_millis = timeout_millis;
    result = kpb_list_with_options(runtime_dir, &options, collect_session, list);
    if (result != KPB_OK || list->allocation_failed) {
        list->count = 0;
        *selected = 0;
        return result != KPB_OK ? result : KPB_ERR_BUFFER;
    }
    if (list->count > 1U) {
        qsort(list->items, list->count, sizeof *list->items, compare_sessions);
    }
    if (list->count == 0) {
        *selected = 0;
        return KPB_OK;
    }
    if (selected_id[0]) {
        for (index = 0; index < list->count; index++) {
            if (strcmp(list->items[index].session_id, selected_id) == 0) {
                *selected = index;
                return KPB_OK;
            }
        }
    }
    if (*selected >= list->count) *selected = list->count - 1U;
    return KPB_OK;
}

static void
terminal_size(unsigned short *rows, unsigned short *columns) {
    struct winsize size;
    memset(&size, 0, sizeof size);
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
        size.ws_row = 24;
        size.ws_col = 80;
    }
    *rows = size.ws_row ? size.ws_row : 24;
    *columns = size.ws_col ? size.ws_col : 80;
}

static int
terminal_enter(terminal_state *state) {
    struct sigaction action;
    struct termios interactive;
    memset(state, 0, sizeof *state);
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &state->saved_termios) != 0) return -1;
    interactive = state->saved_termios;
    interactive.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    interactive.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    interactive.c_cc[VMIN] = 0;
    interactive.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &interactive) != 0) return -1;
    state->have_termios = true;

    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = handle_tui_resize;
    if (sigaction(SIGWINCH, &action, &state->saved_winch) != 0) return -1;
    state->have_winch = true;
    action.sa_handler = handle_tui_stop;
    if (sigaction(SIGHUP, &action, &state->saved_hup) != 0) return -1;
    state->have_hup = true;
    if (sigaction(SIGTERM, &action, &state->saved_term) != 0) return -1;
    state->have_term = true;
    if (sigaction(SIGINT, &action, &state->saved_int) != 0) return -1;
    state->have_int = true;
    fputs("\033[?1049h\033[?25l", stdout);
    fflush(stdout);
    state->alternate_screen = true;
    return 0;
}

static void
terminal_leave(terminal_state *state) {
    if (state->alternate_screen) {
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
        fflush(stdout);
        state->alternate_screen = false;
    }
    if (state->have_int) {
        (void)sigaction(SIGINT, &state->saved_int, NULL);
        state->have_int = false;
    }
    if (state->have_term) {
        (void)sigaction(SIGTERM, &state->saved_term, NULL);
        state->have_term = false;
    }
    if (state->have_hup) {
        (void)sigaction(SIGHUP, &state->saved_hup, NULL);
        state->have_hup = false;
    }
    if (state->have_winch) {
        (void)sigaction(SIGWINCH, &state->saved_winch, NULL);
        state->have_winch = false;
    }
    if (state->have_termios) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &state->saved_termios);
        state->have_termios = false;
    }
}

static void
move_to(unsigned short row, unsigned short column) {
    printf("\033[%u;%uH", row, column);
}

static void
print_sanitized_field(const char *value, unsigned int width) {
    unsigned int used = 0;
    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor && used < width) {
        unsigned char byte = *cursor++;
        putchar(byte >= 0x20U && byte <= 0x7eU ? (int)byte : '?');
        used++;
    }
    while (used++ < width) putchar(' ');
}

static void
format_age(uint64_t started_millis, char output[16]) {
    uint64_t now = realtime_millis();
    uint64_t seconds = now > started_millis ? (now - started_millis) / 1000U : 0;
    if (seconds >= 86400U) {
        uint64_t full_days = seconds / 86400U;
        unsigned int days = full_days > 9999U ? 9999U : (unsigned int)full_days;
        unsigned int hours = (unsigned int)((seconds / 3600U) % 24U);
        snprintf(
            output, 16, "%ud%02uh",
            days,
            hours
        );
    } else if (seconds >= 3600U) {
        snprintf(
            output, 16, "%lluh%02llum",
            (unsigned long long)(seconds / 3600U),
            (unsigned long long)((seconds / 60U) % 60U)
        );
    } else if (seconds >= 60U) {
        snprintf(
            output, 16, "%llum%02llus",
            (unsigned long long)(seconds / 60U),
            (unsigned long long)(seconds % 60U)
        );
    } else {
        snprintf(output, 16, "%llus", (unsigned long long)seconds);
    }
}

static void
format_bytes(uint64_t bytes, char output[20]) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1U < sizeof units / sizeof units[0]) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        snprintf(output, 20, "%llu %s", (unsigned long long)bytes, units[unit]);
    } else {
        snprintf(output, 20, "%.1f %s", value, units[unit]);
    }
}

static size_t
visible_page_start(size_t selected, size_t count, size_t rows) {
    size_t start;
    if (count <= rows || selected < rows) return 0;
    start = selected - rows + 1U;
    if (start + rows > count) start = count - rows;
    return start;
}

/* Seconds for the header: "1s", "0.5s", "2.5s". */
static void
format_seconds(int millis, char output[16]) {
    if (millis % 1000 == 0) snprintf(output, 16, "%ds", millis / 1000);
    else snprintf(output, 16, "%d.%ds", millis / 1000, (millis % 1000) / 100);
}

/* Writes UTF-8 text clipped to `width` display cells.  The text is ours, never
 * session data, so it is not sanitised - but it must not overrun the row, or on
 * the last line the terminal would scroll the whole screen. */
static void
print_clipped(const char *text, unsigned int width) {
    unsigned int cells = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        size_t size = 1;
        if ((*cursor & 0xe0U) == 0xc0U) size = 2;
        else if ((*cursor & 0xf0U) == 0xe0U) size = 3;
        else if ((*cursor & 0xf8U) == 0xf0U) size = 4;
        if (cells >= width) break;
        fwrite(cursor, 1, size, stdout);
        cursor += size;
        cells++;
    }
}

static void
draw_screen(
    const char *runtime_dir,
    int timeout_millis,
    const session_list *list,
    size_t selected,
    const char *message,
    const char *confirmation_id
) {
    unsigned short rows;
    unsigned short columns;
    unsigned int id_width;
    unsigned int command_width;
    unsigned int line_width;
    size_t page_rows;
    size_t start;
    size_t offset;
    size_t attached = 0;
    size_t unreachable = 0;
    terminal_size(&rows, &columns);
    fputs(TUI_RESET "\033[H\033[2J", stdout);

    /*
     * Keep this frame in lockstep with kilix_tui.shell: identity/application,
     * numbered navigation, one divider, one status row, then the body and a
     * quiet footer.  Blue, red, white, and grey are the complete palette.
     */
    line_width = columns > 0U ? (unsigned int)columns - 1U : 0U;
    move_to(1, 2);
    fputs(TUI_TITLE "KILIX TUI" TUI_RESET, stdout);
    if (columns > sizeof "PTY Sessions" + sizeof "KILIX TUI") {
        move_to(1, (unsigned short)(columns - (sizeof "PTY Sessions" - 1U)));
        fputs(TUI_MUTED "PTY Sessions" TUI_RESET, stdout);
    }
    move_to(2, 2);
    fputs(TUI_SELECTED "▶1 Sessions " TUI_RESET, stdout);
    move_to(3, 1);
    fputs(TUI_MUTED, stdout);
    for (offset = 0; offset < line_width; offset++) fputs("─", stdout);
    fputs(TUI_RESET, stdout);

    for (offset = 0; offset < list->count; offset++) {
        if (!list->items[offset].reachable) unreachable++;
        else if (list->items[offset].status.attached) attached++;
    }
    move_to(4, 2);
    if (message && *message) {
        fputs(TUI_ACCENT, stdout);
        print_sanitized_field(
            message, columns > 2U ? (unsigned int)columns - 2U : 0U
        );
        fputs(TUI_RESET, stdout);
    } else {
        char sizing[160];
        char seconds[16];
        char unreachable_text[48] = "";
        int prefix_width;
        format_seconds(timeout_millis, seconds);
        if (unreachable) {
            snprintf(unreachable_text, sizeof unreachable_text, "%zu UNREACHABLE | ", unreachable);
        }
        prefix_width = snprintf(
            sizing,
            sizeof sizing,
            "%zu SESSIONS | %zu ATTACHED | %sTIMEOUT %s | RUNTIME ",
            list->count,
            attached,
            unreachable_text,
            seconds
        );
        fputs(TUI_MUTED, stdout);
        printf(
            "%zu SESSIONS · %zu ATTACHED · ", list->count, attached);
        if (unreachable) printf("%zu UNREACHABLE · ", unreachable);
        printf("TIMEOUT %s · RUNTIME ", seconds);
        print_sanitized_field(
            runtime_dir,
            prefix_width > 0 && (unsigned int)prefix_width + 2U < columns
                ? (unsigned int)columns - (unsigned int)prefix_width - 2U
                : 0U
        );
        fputs(TUI_RESET, stdout);
    }

    if (rows < 12U || columns < 56U) {
        if (rows > 5U) {
            move_to(5, 2);
            fputs(
                TUI_ALERT
                "Terminal must be at least 56 columns by 12 rows."
                TUI_RESET,
                stdout
            );
        }
        move_to(rows, 1);
        fputs(TUI_MUTED " q quit" TUI_RESET, stdout);
        fflush(stdout);
        return;
    }

    id_width = columns / 4U;
    if (id_width < 12U) id_width = 12U;
    if (id_width > 24U) id_width = 24U;
    command_width = columns - id_width - 34U;
    move_to(5, 2);
    fputs(TUI_TITLE "PTY SESSIONS" TUI_RESET, stdout);
    move_to(7, 2);
    fputs(TUI_MUTED "  ", stdout);
    print_sanitized_field("SESSION", id_width);
    fputs("  STATE       AGE      SIZE     COMMAND", stdout);
    fputs(TUI_RESET, stdout);

    page_rows = (size_t)rows - 10U;
    start = visible_page_start(selected, list->count, page_rows);
    for (offset = 0; offset < page_rows; offset++) {
        size_t index = start + offset;
        unsigned short row = (unsigned short)(8U + offset);
        move_to(row, 2);
        if (index >= list->count) {
            print_sanitized_field("", columns - 2U);
            continue;
        }
        {
            const kpb_list_entry *entry = &list->items[index];
            const kpb_status *status = &entry->status;
            char age[16] = "-";
            char dimensions[16] = "-";
            char reason[48];
            const char *state;
            if (entry->reachable) {
                format_age(status->started_millis, age);
                snprintf(
                    dimensions, sizeof dimensions, "%ux%u",
                    status->columns, status->rows
                );
                state = status->attached ? "attached" : "detached";
            } else {
                state = "unreachable";
                snprintf(
                    reason, sizeof reason, "(%s)",
                    entry->error == KPB_ERR_TIMEOUT ? "no answer within the timeout"
                                                    : kpb_result_string(entry->error));
            }
            if (index == selected) fputs(TUI_SELECTED, stdout);
            fputs(index == selected ? "▶ " : "  ", stdout);
            print_sanitized_field(entry->session_id, id_width);
            fputs("  ", stdout);
            print_sanitized_field(state, 11);
            fputc(' ', stdout);
            print_sanitized_field(age, 8);
            print_sanitized_field(dimensions, 9);
            print_sanitized_field(entry->reachable ? status->command : reason, command_width);
            if (index == selected) fputs(TUI_RESET, stdout);
        }
    }

    move_to((unsigned short)(rows - 2U), 2);
    if (list->count && !list->items[selected].reachable) {
        fputs(TUI_ALERT "unreachable" TUI_RESET, stdout);
        move_to((unsigned short)(rows - 1U), 2);
        fputs(
            "Not attachable. It may be stopped or wedged; x asks it to end.",
            stdout
        );
    } else if (list->count) {
        const kpb_status *status = &list->items[selected].status;
        char journal[20];
        char cwd_now[KPB_PATH_MAX];
        format_bytes(status->journal_bytes, journal);
        /* `started in` is where the session began; `now` is read from /proc and
         * is where its command is at the moment. */
        if (kpb_read_cwd_now(status->child_pid, cwd_now, sizeof cwd_now) != KPB_OK ||
            strcmp(cwd_now, status->cwd) == 0) {
            fputs(TUI_MUTED "cwd     " TUI_RESET, stdout);
            print_sanitized_field(
                status->cwd, columns > 9U ? columns - 9U : 0U
            );
        } else {
            fputs(TUI_MUTED "cwd now " TUI_RESET, stdout);
            print_sanitized_field(
                cwd_now, columns > 9U ? columns - 9U : 0U
            );
        }
        move_to((unsigned short)(rows - 1U), 2);
        printf(
            TUI_MUTED "pid     " TUI_RESET
            "%ld · journal %s · replay %s",
            (long)status->child_pid,
            journal,
            status->replay_complete ? "complete" : "partial"
        );
        if (status->journal_epoch) {
            printf(
                "  epoch %llu",
                (unsigned long long)status->journal_epoch
            );
        }
    } else {
        fputs("No persistent PTY sessions are running.", stdout);
        move_to((unsigned short)(rows - 1U), 2);
        fputs("Start a Kilix terminal; it will appear here automatically.", stdout);
    }
    if ((!message || !*message) && list->truncated) {
        move_to(4, 2);
        fputs(TUI_ALERT "Session list truncated." TUI_RESET, stdout);
    }

    move_to(rows, 1);
    if (confirmation_id && confirmation_id[0]) {
        char prompt[TUI_MESSAGE_MAX];
        snprintf(
            prompt,
            sizeof prompt,
            " Terminate %.64s and its process group? · y yes · n cancel",
            confirmation_id
        );
        fputs(TUI_DANGER, stdout);
        print_sanitized_field(prompt, line_width);
        fputs(TUI_RESET, stdout);
    } else {
        /* The full line is 65 cells; the narrowest supported terminal is 56.
         * Writing past the last column on the bottom row would scroll the
         * screen, so a short form is used when the long one does not fit. */
        const char *full = " Enter attach · o observe · x end · ↑/↓ move · r refresh · q quit";
        const char *brief = " Enter attach · o observe · x end · q quit";
        fputs(TUI_MUTED, stdout);
        print_clipped(columns >= 67U ? full : brief, columns > 1U ? columns - 1U : 0U);
        fputs(TUI_RESET, stdout);
    }
    fflush(stdout);
}

static void
select_previous(size_t *selected, size_t count) {
    if (count && *selected > 0) (*selected)--;
}

static void
select_next(size_t *selected, size_t count) {
    if (count && *selected + 1U < count) (*selected)++;
}

static bool
is_sequence(const unsigned char *input, ssize_t count, const char *sequence) {
    size_t size = strlen(sequence);
    return count >= 0 && (size_t)count == size &&
        memcmp(input, sequence, size) == 0;
}

static ssize_t
read_input(unsigned char *input, size_t capacity) {
    ssize_t count;
    if (!capacity) {
        errno = EINVAL;
        return -1;
    }
    count = read(STDIN_FILENO, input, 1);
    if (count != 1 || input[0] != 0x1bU) return count;
    /*
     * Escape sequences can be split across PTY reads. Briefly collect the
     * remainder so an arrow key never looks like a standalone Escape/quit.
     */
    while ((size_t)count < capacity && count < 3) {
        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = POLLIN,
            .revents = 0
        };
        int polled;
        ssize_t extra;
        do {
            polled = poll(&descriptor, 1, 30);
        } while (polled < 0 && errno == EINTR);
        if (polled <= 0 || !(descriptor.revents & POLLIN)) break;
        extra = read(
            STDIN_FILENO,
            input + count,
            (size_t)(3 - count)
        );
        if (extra < 0 && errno == EINTR) continue;
        if (extra <= 0) break;
        count += extra;
    }
    return count;
}

/* What a refresh failure should say.  "session not found" is the library's word
 * for a missing session, and here it would mean the runtime itself vanished. */
static const char *
refresh_failure(kpb_result result) {
    if (result == KPB_ERR_NOT_FOUND) return "the runtime directory is gone";
    return kpb_result_string(result);
}

int
kpb_tui_run(
    const char *runtime_dir,
    int timeout_millis,
    char session_id[KPB_SESSION_ID_MAX + 1]
) {
    session_list list = {0};
    terminal_state terminal;
    size_t selected = 0;
    char message[TUI_MESSAGE_MAX] = "";
    char confirmation_id[KPB_SESSION_ID_MAX + 1] = "";
    bool confirming = false;
    /* The identity of the session the user was shown when they pressed x, so
     * the kill that follows confirmation can only ever end THAT session: the
     * list refreshes while the prompt is up, and a session can be replaced
     * under the same ID in between. */
    bool confirmation_bound = false;
    uint64_t confirmation_started = 0;
    bool redraw = true;
    int result = KPB_TUI_QUIT;
    kpb_result broker_result;

    if (!runtime_dir || !session_id) {
        errno = EINVAL;
        return KPB_TUI_ERROR;
    }
    if (timeout_millis <= 0) timeout_millis = TUI_DEFAULT_TIMEOUT_MILLIS;
    session_id[0] = '\0';
    broker_result = refresh_sessions(runtime_dir, timeout_millis, &list, &selected);
    if (broker_result != KPB_OK) {
        fprintf(
            stderr,
            "kitty-pty-broker: list sessions: %s\n",
            refresh_failure(broker_result)
        );
        free(list.items);
        return KPB_TUI_ERROR;
    }

    tui_resize_pending = 0;
    tui_stop_pending = 0;
    if (terminal_enter(&terminal) != 0) {
        int saved_errno = errno;
        terminal_leave(&terminal);
        fprintf(
            stderr,
            "kitty-pty-broker: tui requires an interactive terminal: %s\n",
            strerror(saved_errno)
        );
        free(list.items);
        return KPB_TUI_ERROR;
    }

    while (!tui_stop_pending) {
        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = POLLIN,
            .revents = 0
        };
        unsigned char input[16];
        ssize_t count;
        int polled;
        if (tui_resize_pending) {
            tui_resize_pending = 0;
            redraw = true;
        }
        if (redraw) {
            draw_screen(
                runtime_dir, timeout_millis, &list, selected, message, confirmation_id
            );
            redraw = false;
        }
        polled = poll(&descriptor, 1, 1000);
        if (polled < 0) {
            if (errno == EINTR) continue;
            snprintf(message, sizeof message, "Input error: %s", strerror(errno));
            result = KPB_TUI_ERROR;
            break;
        }
        if (polled == 0) {
            broker_result = refresh_sessions(runtime_dir, timeout_millis, &list, &selected);
            if (broker_result != KPB_OK) {
                snprintf(
                    message,
                    sizeof message,
                    "Refresh failed: %s",
                    refresh_failure(broker_result)
                );
            }
            redraw = true;
            continue;
        }
        if (!(descriptor.revents & (POLLIN | POLLHUP | POLLERR))) continue;
        count = read_input(input, sizeof input);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            result = KPB_TUI_ERROR;
            break;
        }
        if (count == 0) {
            if (descriptor.revents & (POLLHUP | POLLERR)) break;
            continue;
        }
        message[0] = '\0';

        if (confirming) {
            if (input[0] == 'y' || input[0] == 'Y') {
                char terminated[KPB_SESSION_ID_MAX + 1];
                memcpy(
                    terminated,
                    confirmation_id,
                    sizeof terminated
                );
                terminated[sizeof terminated - 1U] = '\0';
                /* An unreachable entry has no identity to bind to; its kill is
                 * the plain request, which is all the user can mean by it. */
                broker_result = confirmation_bound
                    ? kpb_terminate_expect(
                          runtime_dir, terminated, confirmation_started, timeout_millis)
                    : kpb_terminate_timeout(runtime_dir, terminated, timeout_millis);
                if (broker_result == KPB_OK) {
                    snprintf(
                        message,
                        sizeof message,
                        "Termination requested for %.64s.",
                        terminated
                    );
                } else if (broker_result == KPB_ERR_TIMEOUT) {
                    snprintf(
                        message,
                        sizeof message,
                        "%.64s did not answer; it may still act on the request.",
                        terminated
                    );
                } else if (broker_result == KPB_ERR_MISMATCH) {
                    snprintf(
                        message,
                        sizeof message,
                        "%.64s is now a different session (it was replaced); nothing was done.",
                        terminated
                    );
                } else if (broker_result == KPB_ERR_UNSUPPORTED) {
                    snprintf(
                        message,
                        sizeof message,
                        "%.64s is from an older build and cannot verify it is the one you "
                        "chose; nothing was done (kitty-pty-broker kill ID ends it).",
                        terminated
                    );
                } else {
                    snprintf(
                        message,
                        sizeof message,
                        "Could not terminate %.64s: %s",
                        terminated,
                        kpb_result_string(broker_result)
                    );
                }
                (void)refresh_sessions(runtime_dir, timeout_millis, &list, &selected);
                confirming = false;
                confirmation_bound = false;
                confirmation_id[0] = '\0';
            } else if (
                input[0] == 'n' || input[0] == 'N' ||
                input[0] == 'q' || input[0] == 0x1bU
            ) {
                confirming = false;
                confirmation_id[0] = '\0';
                snprintf(message, sizeof message, "Termination cancelled.");
            }
            redraw = true;
            continue;
        }

        if (input[0] == 'q' || input[0] == 'Q' || input[0] == 0x03U ||
            (input[0] == 0x1bU && count == 1)) {
            break;
        }
        if (input[0] == 'k' || is_sequence(input, count, "\033[A")) {
            select_previous(&selected, list.count);
            redraw = true;
        } else if (input[0] == 'j' || is_sequence(input, count, "\033[B")) {
            select_next(&selected, list.count);
            redraw = true;
        } else if (input[0] == 'g' || is_sequence(input, count, "\033[H")) {
            selected = 0;
            redraw = true;
        } else if (input[0] == 'G' || is_sequence(input, count, "\033[F")) {
            if (list.count) selected = list.count - 1U;
            redraw = true;
        } else if (input[0] == 'r' || input[0] == 'R') {
            broker_result = refresh_sessions(runtime_dir, timeout_millis, &list, &selected);
            if (broker_result == KPB_OK) {
                snprintf(message, sizeof message, "Session list refreshed.");
            } else {
                snprintf(
                    message,
                    sizeof message,
                    "Refresh failed: %s",
                    refresh_failure(broker_result)
                );
            }
            redraw = true;
        } else if (input[0] == 'x' || input[0] == 'X' || input[0] == 'd') {
            if (list.count) {
                confirming = true;
                memcpy(
                    confirmation_id,
                    list.items[selected].session_id,
                    sizeof confirmation_id
                );
                confirmation_id[sizeof confirmation_id - 1U] = '\0';
                confirmation_bound = list.items[selected].reachable != 0;
                confirmation_started = confirmation_bound
                    ? list.items[selected].status.started_millis : 0;
            } else {
                snprintf(message, sizeof message, "There is no session to terminate.");
            }
            redraw = true;
        } else if (
            input[0] == '\r' || input[0] == '\n' ||
            input[0] == 'o' || input[0] == 'O'
        ) {
            bool observing = input[0] == 'o' || input[0] == 'O';
            if (!list.count) {
                snprintf(
                    message, sizeof message,
                    observing ? "There is no session to observe."
                              : "There is no session to attach."
                );
                redraw = true;
            } else if (!list.items[selected].reachable) {
                snprintf(
                    message,
                    sizeof message,
                    "%.64s is unreachable and cannot be %s.",
                    list.items[selected].session_id,
                    observing ? "observed" : "attached"
                );
                redraw = true;
            } else if (!observing && list.items[selected].status.attached) {
                snprintf(
                    message,
                    sizeof message,
                    "%.64s is already attached; o observes it read-only.",
                    list.items[selected].session_id
                );
                redraw = true;
            } else {
                /* Observing is read-only and allowed for an attached pane. */
                memcpy(
                    session_id,
                    list.items[selected].session_id,
                    KPB_SESSION_ID_MAX + 1U
                );
                session_id[KPB_SESSION_ID_MAX] = '\0';
                result = observing ? KPB_TUI_OBSERVE : KPB_TUI_ATTACH;
                break;
            }
        }
    }

    terminal_leave(&terminal);
    free(list.items);
    return result;
}
