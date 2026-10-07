#ifndef KITTY_PTY_BROKER_TUI_H
#define KITTY_PTY_BROKER_TUI_H

#include "kitty_pty_broker.h"

enum {
    KPB_TUI_QUIT = 0,
    KPB_TUI_ATTACH = 1,
    KPB_TUI_OBSERVE = 2,
    KPB_TUI_ERROR = -1
};

/* Run the session manager.  `timeout_millis` bounds each list, kill and the
 * handshakes it triggers (<= 0: one second).  On KPB_TUI_ATTACH or
 * KPB_TUI_OBSERVE, `session_id` names the chosen session. */
int kpb_tui_run(
    const char *runtime_dir,
    int timeout_millis,
    char session_id[KPB_SESSION_ID_MAX + 1]
);

#endif
