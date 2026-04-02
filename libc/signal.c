#include <errno.h>
#include <signal.h>
#include <syscall.h>

_syscall2(int,kill,int,pid,int,sig)
_syscall3(int,sigaction,int,signum,const struct sigaction *,act, struct sigaction *,oldact)
_syscall1(int,sigsuspend,const sigset_t *,mask)
_syscall1(int,sigpending,sigset_t *,set)
_syscall3(int,sigprocmask,int,how,const sigset_t *,set,sigset_t *,oldset)
_syscall2(int,signal,int,signum,sighandler_t,handler)

int sigdelset(sigset_t *set, int sig) {
    unsigned s = sig - 1;
    if (s >= _NSIG - 1 || sig - 32U < 3) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1UL << sig);
    return 0;
}

int sigaddset(sigset_t *set, int sig) {
    unsigned s = sig - 1;
    if (s >= _NSIG - 1 || sig - 32U < 3) {
        errno = EINVAL;
        return -1;
    }
    *set |= (1 << sig);
    return 0;
}

#define SST_SIZE (_NSIG / 8 / sizeof(long))

int sigandset(sigset_t *dest, const sigset_t *left, const sigset_t *right) {
    *dest = *left & *right;
    return 0;
}

int sigemptyset(sigset_t *set) {
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    *set = 0xfffffffful;
    return 0;
}

int sigisemptyset(const sigset_t *set) {
    return *set == 0;
}

int sigismember(const sigset_t *set, int sig) {
    unsigned s = sig - 1;
    if (s >= _NSIG - 1)
        return 0;
    return !!(*set & (1 << sig));
}
