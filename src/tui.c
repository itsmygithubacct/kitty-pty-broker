#define _POSIX_C_SOURCE 200809L

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
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TUI_SESSION_LIMIT 2048U
#define TUI_MESSAGE_MAX 256U

typedef struct {
    kpb_status *items;
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

static uint64_t
realtime_millis(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int
collect_session(const kpb_status *status, void *opaque) {
    session_list *list = opaque;
    kpb_status *resized;
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
    list->items[list->count++] = *status;
    return 0;
}

static int
compare_sessions(const void *left_opaque, const void *right_opaque) {
    const kpb_status *left = left_opaque;
    const kpb_status *right = right_opaque;
    if (!!left->attached != !!right->attached) {
        return left->attached ? 1 : -1;
    }
    if (left->started_millis < right->started_millis) return 1;
    if (left->started_millis > right->started_millis) return -1;
    return strcmp(left->session_id, right->session_id);
}

static kpb_result
refresh_sessions(
    const char *runtime_dir,
    session_list *list,
    size_t *selected
) {
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
    result = kpb_list(runtime_dir, collect_session, list);
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

static void
draw_screen(
    const char *runtime_dir,
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
    terminal_size(&rows, &columns);
    fputs("\033[H\033[2J", stdout);
    if (rows < 12U || columns < 56U) {
        move_to(1, 1);
        fputs("\033[1;7m PTY Sessions \033[0m", stdout);
        move_to(3, 1);
        fputs("Terminal must be at least 56 columns by 12 rows.", stdout);
        move_to(rows, 1);
        fputs("q: quit", stdout);
        fflush(stdout);
        return;
    }

    line_width = columns;
    move_to(1, 1);
    fputs("\033[1;7m", stdout);
    print_sanitized_field(" Kilix PTY Sessions", line_width);
    fputs("\033[0m", stdout);
    move_to(2, 1);
    fputs("Runtime: ", stdout);
    print_sanitized_field(runtime_dir, columns > 9U ? columns - 9U : 0U);

    id_width = columns / 4U;
    if (id_width < 12U) id_width = 12U;
    if (id_width > 24U) id_width = 24U;
    command_width = columns - id_width - 31U;
    move_to(4, 1);
    fputs("  ", stdout);
    print_sanitized_field("SESSION", id_width);
    fputs("  STATE     AGE      SIZE     COMMAND", stdout);

    page_rows = (size_t)rows - 9U;
    start = visible_page_start(selected, list->count, page_rows);
    for (offset = 0; offset < page_rows; offset++) {
        size_t index = start + offset;
        unsigned short row = (unsigned short)(5U + offset);
        move_to(row, 1);
        if (index >= list->count) {
            print_sanitized_field("", columns);
            continue;
        }
        {
            const kpb_status *status = &list->items[index];
            char age[16];
            char size[20];
            char dimensions[16];
            format_age(status->started_millis, age);
            format_bytes(status->journal_bytes, size);
            snprintf(
                dimensions, sizeof dimensions, "%ux%u",
                status->columns, status->rows
            );
            if (index == selected) fputs("\033[7m", stdout);
            fputs(index == selected ? "> " : "  ", stdout);
            print_sanitized_field(status->session_id, id_width);
            fputs("  ", stdout);
            print_sanitized_field(
                status->attached ? "attached" : "detached", 9
            );
            fputc(' ', stdout);
            print_sanitized_field(age, 8);
            print_sanitized_field(dimensions, 9);
            print_sanitized_field(status->command, command_width);
            if (index == selected) fputs("\033[0m", stdout);
        }
    }

    move_to((unsigned short)(rows - 3U), 1);
    if (list->count) {
        const kpb_status *status = &list->items[selected];
        char journal[20];
        format_bytes(status->journal_bytes, journal);
        fputs("cwd: ", stdout);
        print_sanitized_field(status->cwd, columns > 5U ? columns - 5U : 0U);
        move_to((unsigned short)(rows - 2U), 1);
        printf(
            "pid %ld  journal %s  replay %s",
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
        move_to((unsigned short)(rows - 2U), 1);
        fputs("Start a Kilix terminal; it will appear here automatically.", stdout);
    }
    if (message && *message) {
        move_to((unsigned short)(rows - 1U), 1);
        print_sanitized_field(message, columns);
    } else if (list->truncated) {
        move_to((unsigned short)(rows - 1U), 1);
        fputs("Session list truncated.", stdout);
    }

    move_to(rows, 1);
    fputs("\033[7m", stdout);
    if (confirmation_id && confirmation_id[0]) {
        char prompt[TUI_MESSAGE_MAX];
        snprintf(
            prompt,
            sizeof prompt,
            " Terminate %.64s and its process group?  y: yes  n: cancel",
            confirmation_id
        );
        print_sanitized_field(prompt, line_width);
    } else {
        print_sanitized_field(
            " Enter: attach   x: terminate   r: refresh   j/k or arrows: move   q: quit",
            line_width
        );
    }
    fputs("\033[0m", stdout);
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
        int polled = poll(&descriptor, 1, 30);
        ssize_t extra;
        if (polled <= 0 || !(descriptor.revents & POLLIN)) break;
        extra = read(
            STDIN_FILENO,
            input + count,
            (size_t)(3 - count)
        );
        if (extra <= 0) break;
        count += extra;
    }
    return count;
}

int
kpb_tui_run(
    const char *runtime_dir,
    char session_id[KPB_SESSION_ID_MAX + 1]
) {
    session_list list = {0};
    terminal_state terminal;
    size_t selected = 0;
    char message[TUI_MESSAGE_MAX] = "";
    char confirmation_id[KPB_SESSION_ID_MAX + 1] = "";
    bool confirming = false;
    bool redraw = true;
    int result = KPB_TUI_QUIT;
    kpb_result broker_result;

    if (!runtime_dir || !session_id) {
        errno = EINVAL;
        return KPB_TUI_ERROR;
    }
    session_id[0] = '\0';
    broker_result = refresh_sessions(runtime_dir, &list, &selected);
    if (broker_result != KPB_OK) {
        fprintf(
            stderr,
            "kitty-pty-broker: list sessions: %s\n",
            kpb_result_string(broker_result)
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
                runtime_dir, &list, selected, message, confirmation_id
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
            broker_result = refresh_sessions(runtime_dir, &list, &selected);
            if (broker_result != KPB_OK) {
                snprintf(
                    message,
                    sizeof message,
                    "Refresh failed: %s",
                    kpb_result_string(broker_result)
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
                broker_result = kpb_terminate(runtime_dir, terminated);
                if (broker_result == KPB_OK) {
                    snprintf(
                        message,
                        sizeof message,
                        "Termination requested for %.64s.",
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
                (void)refresh_sessions(runtime_dir, &list, &selected);
                confirming = false;
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
            broker_result = refresh_sessions(runtime_dir, &list, &selected);
            if (broker_result == KPB_OK) {
                snprintf(message, sizeof message, "Session list refreshed.");
            } else {
                snprintf(
                    message,
                    sizeof message,
                    "Refresh failed: %s",
                    kpb_result_string(broker_result)
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
            } else {
                snprintf(message, sizeof message, "There is no session to terminate.");
            }
            redraw = true;
        } else if (input[0] == '\r' || input[0] == '\n') {
            if (!list.count) {
                snprintf(message, sizeof message, "There is no session to attach.");
                redraw = true;
            } else if (list.items[selected].attached) {
                snprintf(
                    message,
                    sizeof message,
                    "%.64s is already attached.",
                    list.items[selected].session_id
                );
                redraw = true;
            } else {
                memcpy(
                    session_id,
                    list.items[selected].session_id,
                    KPB_SESSION_ID_MAX + 1U
                );
                session_id[KPB_SESSION_ID_MAX] = '\0';
                result = KPB_TUI_ATTACH;
                break;
            }
        }
    }

    terminal_leave(&terminal);
    free(list.items);
    return result;
}
