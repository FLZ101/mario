#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <syscall.h>
#include <sys/types.h>
#include <time.h>

static inline _syscall5(int,_newselect,int,nfds, fd_set *,readfds,
    fd_set *,writefds, fd_set *,exceptfds, struct timeval *,timeout)

static inline int select(int nfds, fd_set *readfds, fd_set *writefds,
    fd_set *exceptfds, struct timeval *timeout)
{
    return _newselect(nfds, readfds, writefds, exceptfds, timeout);
}

#endif
