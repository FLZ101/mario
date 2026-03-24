#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[])
{
	int err;
	int pid;
	int pipefd[2];

	if ((err = pipe(pipefd))) {
		perror("pipe");
		return 1;
	}

	pid = fork();
	if (-1 == pid) {
		perror("fork");
		return 1;
	}

	char message[] = "The quick brown fox jumps over the lazy dog";

	if (pid) {
		int n = strlen(message);
		memset(message, 0, n);

		printf("[%d] waiting message from %d\n", getpid(), pid);
		read(pipefd[0], message, n);
		printf("[%d] got message from %d: %s\n", getpid(), pid, message);
	} else {
		sleep(3);
		write(pipefd[1], message, strlen(message));
		printf("[%d] sent message to %d\n", getpid(), getppid());
	}
	return 0;
}
