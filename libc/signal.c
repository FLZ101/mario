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
    set->__bits[s / 8 / sizeof *set->__bits] &=
        ~(1UL << (s & (8 * sizeof *set->__bits - 1)));
    return 0;
}

int sigaddset(sigset_t *set, int sig) {
    unsigned s = sig - 1;
    if (s >= _NSIG - 1 || sig - 32U < 3) {
        errno = EINVAL;
        return -1;
    }
    set->__bits[s / 8 / sizeof *set->__bits] |=
        1UL << (s & (8 * sizeof *set->__bits - 1));
    return 0;
}

#define SST_SIZE (_NSIG / 8 / sizeof(long))

int sigandset(sigset_t *dest, const sigset_t *left, const sigset_t *right) {
    unsigned long i = 0, *d = (void *)dest, *l = (void *)left,
                  *r = (void *)right;
    for (; i < SST_SIZE; i++)
        d[i] = l[i] & r[i];
    return 0;
}

int sigemptyset(sigset_t *set) {
    set->__bits[0] = 0;
    if (sizeof(long) == 4 || _NSIG > 65)
        set->__bits[1] = 0;
    if (sizeof(long) == 4 && _NSIG > 65) {
        set->__bits[2] = 0;
        set->__bits[3] = 0;
    }
    return 0;
}

int sigfillset(sigset_t *set) {
    set->__bits[0] = 0x7ffffffful;
    set->__bits[1] = 0xfffffffcul;
    if (_NSIG > 65) {
        set->__bits[2] = 0xfffffffful;
        set->__bits[3] = 0xfffffffful;
    }
    return 0;
}

int sigisemptyset(const sigset_t *set) {
    for (size_t i = 0; i < _NSIG / 8 / sizeof *set->__bits; i++)
        if (set->__bits[i])
            return 0;
    return 1;
}

int sigismember(const sigset_t *set, int sig) {
    unsigned s = sig - 1;
    if (s >= _NSIG - 1)
        return 0;
    return !!(set->__bits[s / 8 / sizeof *set->__bits] &
              1UL << (s & (8 * sizeof *set->__bits - 1)));
}
