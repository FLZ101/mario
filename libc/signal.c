#include <syscall.h>
#include <signal.h>

_syscall2(int,kill,int,pid,int,sig)
_syscall3(int,sigaction,int,signum,const struct sigaction *,act, struct sigaction *,oldact)
