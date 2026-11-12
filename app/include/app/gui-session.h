#ifndef APP_GUI_SESSION_H
#define APP_GUI_SESSION_H

#include <unistd.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define WM_STARTED_PATH "/tmp/mario-wm.started"
#define WM_READY_PATH "/tmp/mario-wm.ready"
#define WM_FRAME_PATH "/tmp/mario-wm.frame"

static inline void gui_session_remove_status_files(void)
{
    unlink(WM_SOCKET_PATH);
    unlink(WM_READY_PATH);
    unlink(WM_FRAME_PATH);
    unlink(WM_READY_PATH ".new");
    unlink(WM_FRAME_PATH ".new");
    unlink(WM_STARTED_PATH);
}

#endif
