#ifndef _SIGNAL_H
#define _SIGNAL_H

#define SIGHUP		1
#define SIGINT		2
#define SIGQUIT		3
#define SIGILL		4
#define SIGTRAP		5
#define SIGABRT		6
#define SIGBUS		7
#define SIGFPE		8
#define SIGKILL		9
#define SIGUSR1		10
#define SIGSEGV		11
#define SIGUSR2		12
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGSTKFLT	16
#define SIGCHLD		17
#define SIGCONT		18
#define SIGSTOP		19
#define SIGTSTP		20
#define SIGTTIN		21
#define SIGTTOU		22
#define SIGURG		23
#define SIGXCPU		24
#define SIGXFSZ		25
#define SIGVTALRM	26
#define SIGPROF		27
#define SIGWINCH	28
#define SIGIO		29
#define SIGPWR		30
#define	SIGUNUSED	31

#define SIGPOLL		SIGIO

#define _NSIG		32
#define NSIG		_NSIG

#define SA_NOCLDSTOP  1
#define SA_NOCLDWAIT  2
#define SA_SIGINFO    4
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000
#define SA_RESTORER   0x04000000

#define SA_ONESHOT	SA_RESETHAND
#define SA_NOMASK	SA_NODEFER

#define SIG_BLOCK          0
#define SIG_UNBLOCK        1
#define SIG_SETMASK        2

/* Type of a signal handler.  */
typedef void (*__sighandler_t)(int);

typedef unsigned long sigset_t;		/* at least 32 bits */

#define SIG_DFL	((__sighandler_t)0)	/* default signal handling */
#define SIG_IGN	((__sighandler_t)1)	/* ignore signal */
#define SIG_ERR	((__sighandler_t)-1)	/* error return from signal */

struct sigaction {
	__sighandler_t sa_handler;
	unsigned long sa_flags;
	void (*sa_restorer)(void);
	sigset_t sa_mask;
};

#ifdef __KERNEL__

#define signal_pending(p)      ((p)->signal & ~(p)->blocked)

struct task_struct;

int send_sig(unsigned long sig, struct task_struct *p, int priv);
int kill_pg(int pgrp, int sig, int priv);
int kill_sl(int session, int sig, int priv);
int kill_proc(int pid, int sig, int priv);

struct sigcontext_struct {
	unsigned short gs, __gsh;
	unsigned short fs, __fsh;
	unsigned short es, __esh;
	unsigned short ds, __dsh;
	unsigned long edi;
	unsigned long esi;
	unsigned long ebp;
	unsigned long esp;
	unsigned long ebx;
	unsigned long edx;
	unsigned long ecx;
	unsigned long eax;
	unsigned long trapno;
	unsigned long err;
	unsigned long eip;
	unsigned short cs, __csh;
	unsigned long eflags;
	unsigned long esp_at_signal;
	unsigned short ss, __ssh;
	unsigned long i387;
	unsigned long old_mask;
	unsigned long cr2;
};

#endif	/* __KERNEL__ */

#endif	/* _SIGNAL_H */
