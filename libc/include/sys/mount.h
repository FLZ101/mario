#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#include <syscall.h>

static inline _syscall5(int,mount,char *,dev_name,char *,dir_name,char *,type,
    unsigned long,mountflags, void *,data)
static inline _syscall1(int,umount,char *,name)

#endif /* _SYS_MOUNT_H */
