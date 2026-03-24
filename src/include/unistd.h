#ifndef _UNISTD_H
#define _UNISTD_H

#include <syscall.h>
#include <types.h>

#ifdef __KERNEL_SYSCALLS__
static inline _syscall3(int,execve,const char *,file,char **,argv,char **,envp)
#endif	/* __KERNEL_SYSCALLS__ */

#endif	/* _UNISTD_H */
