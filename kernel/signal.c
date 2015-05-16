#include <signal.h>
#include <task.h>
#include <misc.h>

char *message[32] = {
	"SIGHUP",
	"SIGINT",
	"SIGQUIT",
	"SIGILL",
	"SIGTRAP",
	"SIGABRT",
	"SIGBUS",
	"SIGFPE",
	"SIGKILL",
	"SIGUSR1",
	"SIGSEGV",
	"SIGUSR2",
	"SIGPIPE",
	"SIGALRM",
	"SIGTERM",
	"SIGSTKFLT",
	"SIGCHLD",
	"SIGCONT",
	"SIGSTOP",
	"SIGTSTP",
	"SIGTTIN",
	"SIGTTOU",
	"SIGURG",
	"SIGXCPU",
	"SIGXFSZ",
	"SIGVTALRM",
	"SIGPROF",
	"SIGWINCH",
	"SIGIO",
	"SIGPWR",
	"SIGUNUSED"
};

int send_sig(unsigned long sig,struct task_struct *p,int priv)
{
	if (!p || sig > 32)
		return -EINVAL;
	if (!sig)
		return 0;
	early_print("\tsignal: %s\n", message[sig-1]);
	return 0;
}