#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>

#include <app/util.h>

int main(int argc, char *argv[], char *envp[])
{
	if (argc < 2)
		Exit();

	setsid();

	int fd = open(argv[1], O_RDWR);
	if (-1 == fd)
		Exit();

	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	RunL("/bin/kilo.exe", "/root/main.c", NULL);

	while (1)
		pause();
}
