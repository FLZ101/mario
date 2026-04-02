#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <app/util.h>
#include <time.h>

volatile sig_atomic_t done = 0;

static void f(int sig)
{
	// NOT async-signal-safe
	printf("[%d] sig:%d\n", getpid(), sig);

	if (sig == SIGUSR2)
		done = 1;
}

int main(int argc, char *argv[])
{
	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = f;
	sa.sa_restorer = NULL;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);

	kill(getpid(), SIGUSR1);

	int pid = fork();
	HandleErr(pid);

	if (!pid) {
		kill(getppid(), SIGUSR2);
	} else {
		kill(pid, SIGUSR2);
	}

	while (!done)
		pause();

	printf("[%d] exit\n", getpid());
	sleep(1);
	return 0;
}
