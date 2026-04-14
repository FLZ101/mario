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

	if (pid) {
		close(pipefd[1]);

		char ch;
		while (1) {
			int ret = read(pipefd[0], &ch, 1);
			if (-1 == ret) {
				perror("read");
				return 1;
			}

			// EOF
			if (ret == 0)
				break;
			putchar(ch);
		}
		close(pipefd[0]);
	} else {
		close(pipefd[0]);

		char c = '\n';
		char *s;
		char *messages[] = {"See", "you", "tomorrow", NULL};
		for (int i = 0; (s = messages[i]); ++i) {
			sleep(1);
			write(pipefd[1], s, strlen(s));
			write(pipefd[1], &c, 1);
		}
		close(pipefd[1]);
	}
	printf("Exit. %d\n", getpid());
	return 0;
}
