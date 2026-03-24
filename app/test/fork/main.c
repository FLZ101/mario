#include <stdio.h>
#include <unistd.h>
#include <app/util.h>

int main(int argc, char *argv[])
{
	int pid = fork();
	HandleErr(pid);

	if (pid) {
		printf("child: %d\n", pid);
	} else {
		printf("parent: %d\n", getppid());
	}
	return 0;
}
