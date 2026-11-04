#ifndef _FS_FBDEV_H
#define _FS_FBDEV_H

/* Called by fbcon only after the requested foreground frame is visible. */
void fbdev_console_state_changed(int graphics);

#endif /* _FS_FBDEV_H */
