#include <syscall.h>
#include <signal.h>

_syscall2(int,kill,int,pid,int,sig)
_syscall3(int,sigaction,int,signum,const struct sigaction *,act, struct sigaction *,oldact)
_syscall1(int,sigsuspend,const sigset_t *,mask)
_syscall1(int,sigpending,sigset_t *,set)
_syscall3(int,sigprocmask,int,how,const sigset_t *,set,sigset_t *,oldset)
