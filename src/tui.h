#ifndef KITTY_PTY_BROKER_TUI_H
#define KITTY_PTY_BROKER_TUI_H

#include "kitty_pty_broker.h"

enum {
    KPB_TUI_QUIT = 0,
    KPB_TUI_ATTACH = 1,
    KPB_TUI_ERROR = -1
};

int kpb_tui_run(
    const char *runtime_dir,
    char session_id[KPB_SESSION_ID_MAX + 1]
);

#endif
