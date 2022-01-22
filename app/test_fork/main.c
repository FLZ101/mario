#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	int pid;

	if ((pid = fork())) {
		printf("+ %d\n", pid);
	} else {
		printf("* %d\n", getppid());
	}
	return 0;
}
