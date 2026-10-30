#ifndef _VIDEO_FBCON_H
#define _VIDEO_FBCON_H

struct console;

void fbcon_start(void);
void fbcon_console_changed(struct console *con);
void fbcon_console_switched(struct console *con);
int fbcon_enabled(void);

#endif
