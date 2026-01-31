#include <app/util.h>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>

void cat(char *filename)
{
	int err;

	int fd = open(filename, O_RDONLY);
	if (-1 == fd) {
		_perror();
		exit(EXIT_FAILURE);
	}

	char ch;
	while (1) {
		int ret = read(fd, &ch, 1);
		if (-1 == ret) {
			_perror();
			exit(EXIT_FAILURE);
		}
		// EOF
		if (ret == 0)
			break;
		putchar(ch);
	}

	err = close(fd);
	if (-1 == err) {
		_perror();
		exit(EXIT_FAILURE);
	}
}

void run(char *filename)
{
	int err;
	pid_t pid;

	printf("[run] %s\n", filename);

	pid = fork();
	if (-1 == pid) {
		_perror();
		exit(EXIT_FAILURE);
	}

	if (!pid) {
		// NOTE: argv and envp can not contain "", or EFAULT happens
		static char *argv[] = { NULL };
		static char *envp[] = { NULL };

		err = execve(filename, argv, envp);
		if (-1 == err) {
			_perror();
			exit(EXIT_FAILURE);
		}
	} else {
		int status = 0;
		err = waitpid(pid, &status, 0);
		if (-1 == err) {
			_perror();
			exit(EXIT_FAILURE);
		}
		if (WIFEXITED(status)) {
			printf("[run] status = %d\n", WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			printf("[run] signal = %d\n", WTERMSIG(status));
		}
	}
}
