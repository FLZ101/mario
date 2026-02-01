#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <syscall.h>

static inline _syscall3(int,ioctl,int,fd,unsigned int,cmd,unsigned long,arg)

#endif
