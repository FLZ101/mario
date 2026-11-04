#ifndef _FS_GUI_H
#define _FS_GUI_H

/*
 * The graphics seat exists only while one process owns both /dev/gui and a
 * writable /dev/fb0 description.  The framebuffer driver reports its writer
 * lifetime here; the console uses gui_seat_ready() to gate F8 transitions.
 */
void gui_init(void);
int gui_seat_ready(void);
void gui_fb_writer_open(void);
void gui_fb_writer_release(void);

#endif /* _FS_GUI_H */
