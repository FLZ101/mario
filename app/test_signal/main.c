#include <stdio.h>
#include <unistd.h>
#include <signal.h>

static void f(int sig)
{
	printf("[%d, %d]\n", getpid(), sig);
}

int main(int argc, char *argv[])
{
	struct sigaction sa;

	sa.sa_mask = 0;
	sa.sa_flags = 0;
	sa.sa_handler = f;
	sa.sa_restorer = NULL;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);

	kill(getpid(), SIGUSR1);
	printf(":-)\n");

	int pid;
	if (!(pid = fork())) {
		kill(getppid(), SIGUSR2);
		pause();
		return -1;
	}
	kill(pid, SIGUSR1);
	while (1)
		pause();
}
