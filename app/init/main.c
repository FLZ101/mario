#include <app/util.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t should_exit = 0;

void sig_handler(int sig) {
	if (sig == SIGCHLD) {
		// reap zombies
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
	} else if (sig == SIGTERM || sig == SIGINT) {
		should_exit = 1;
	}
}

int main(int argc, char *argv[], char *envp[])
{

#define HANDLE_ERR(x) \
do { \
	if (-1 == (x)) { \
		_perror(); \
		goto tail; \
	} \
} while (0)

	HANDLE_ERR(setsid());
	HANDLE_ERR(open("/dev/tty1", O_RDWR));
	HANDLE_ERR(dup(0));
	HANDLE_ERR(dup(0));

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

	HANDLE_ERR(sigaction(SIGCHLD, &sa, NULL));
	HANDLE_ERR(sigaction(SIGTERM, &sa, NULL));
	HANDLE_ERR(sigaction(SIGINT, &sa, NULL));

	HANDLE_ERR(signal(SIGPIPE, SIG_IGN));
	HANDLE_ERR(signal(SIGHUP, SIG_IGN));
	HANDLE_ERR(signal(SIGQUIT, SIG_IGN));

	cat("/etc/welcome.txt");

	run("/bin/cat.exe");

tail:
	while (1) {
		pause();
	}
	return 0;
}
