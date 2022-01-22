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

#define SA_NOCLDSTOP	1
#define SA_RESTART		0x10000000
#define SA_NOMASK		0x20000000
#define SA_ONESHOT		0x40000000

#define SA_RESETHAND	SA_ONESHOT
#define SA_NODEFER		SA_NOMASK

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
	sigset_t sa_mask;
	unsigned long sa_flags;
	void (*sa_restorer)(void);
};

#include <unistd.h>
int kill(pid_t pid, int sig);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#endif	/* _SIGNAL_H */
