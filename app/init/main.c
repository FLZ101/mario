#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <wait.h>

void run(char *filename)
{
	int err;
	pid_t pid;

	printf("run %s\n", filename);

	pid = fork();
	if (-1 == pid) {
		perror(NULL);
		exit(EXIT_FAILURE);
	}

	if (!pid) {
		// NOTE: argv and envp can not contain "", or EFAULT happens
		static char *argv[] = { NULL };
		static char *envp[] = { NULL };

		err = execve(filename, argv, envp);
		if (-1 == err) {
			perror(NULL);
			exit(EXIT_FAILURE);
		}
	} else {
		int status = 0;
		err = waitpid(pid, &status, 0);
		if (-1 == err) {
			perror(NULL);
			exit(EXIT_FAILURE);
		}
		if (WIFEXITED(status)) {
			printf("status = %d\n", WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			printf("signal = %d\n", WTERMSIG(status));
		}
	}
}

int main(int argc, char *argv[], char *envp[])
{
	int fd = open("/etc/welcome.txt", O_RDONLY);
	if (-1 == fd) {
		perror(NULL);
		exit(EXIT_FAILURE);
	}

	char ch;
	while (1) {
		int ret = read(fd, &ch, 1);
		if (-1 == ret) {
			perror(NULL);
			exit(EXIT_FAILURE);
		}
		// EOF
		if (ret == 0)
			break;
		putchar(ch);
	}

	run("/bin/test_pipe.exe");
	return 0;
}
