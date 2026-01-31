#ifndef _MOUNT_H
#define _MOUNT_H

#include <syscall.h>

static inline _syscall3(int,mount,char *,dev_name,char *,dir_name,char *,type)
static inline _syscall1(int,umount,char *,name)

#endif /* _MOUNT_H */
