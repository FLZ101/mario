#ifndef _FS_VMMOUSE_H
#define _FS_VMMOUSE_H

struct mario_psaux_event;

int vmmouse_init(void);
int vmmouse_read_event(struct mario_psaux_event *event);

#endif
