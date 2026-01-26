#ifndef _WAIT_H
#define _WAIT_H

#include <types.h>
#include <syscall.h>

static inline _syscall3(pid_t,waitpid,pid_t,pid,int *,wstatus,int,options)

/* If WIFEXITED(STATUS), the low-order 8 bits of the status.  */
#define	WEXITSTATUS(status) (((status) & 0xff00) >> 8)

/* If WIFSIGNALED(STATUS), the terminating signal.  */
#define	WTERMSIG(status)    ((status) & 0x7f)

/* If WIFSTOPPED(STATUS), the signal that stopped the child.  */
#define	WSTOPSIG(status)	WEXITSTATUS(status)

/* Nonzero if STATUS indicates normal termination.  */
#define	WIFEXITED(status)	(WTERMSIG(status) == 0)

/* Nonzero if STATUS indicates termination by a signal.  */
#define WIFSIGNALED(status) \
  (((signed char) (((status) & 0x7f) + 1) >> 1) > 0)

/* Nonzero if STATUS indicates the child is stopped.  */
#define	WIFSTOPPED(status)	(((status) & 0xff) == 0x7f)

#endif /* _WAIT_H */
