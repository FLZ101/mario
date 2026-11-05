#ifndef GUI_EVENT_H
#define GUI_EVENT_H

#include <stdint.h>

typedef unsigned int gui_event_handle;

#define GUI_EVENT_HANDLE_NONE 0U

typedef struct gui_event_loop gui_event_loop;

typedef void (*gui_event_fd_fn)(void *context, short revents);
typedef void (*gui_event_timer_fn)(void *context);

gui_event_loop *gui_event_loop_create(void);
void gui_event_loop_destroy(gui_event_loop *loop);

int gui_event_loop_now_msec(uint64_t *msec);

gui_event_handle gui_event_loop_add_fd(gui_event_loop *loop, int fd,
				       short events, gui_event_fd_fn callback,
				       void *context);
gui_event_handle gui_event_loop_add_timer(gui_event_loop *loop,
					  uint64_t deadline_msec,
					  gui_event_timer_fn callback,
					  void *context);
int gui_event_loop_remove(gui_event_loop *loop, gui_event_handle handle);

/* Returns dispatched callbacks, zero when idle, or -1 on a system error. */
int gui_event_loop_run_once(gui_event_loop *loop);

#endif
