#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <app/util.h>

int main(int argc, char *argv[])
{
	printf("[%d] fork\n", getpid());

	int pid = fork();
	HandleErr(pid);

	if (pid) {
		printf("[%d] The child is %d\n", getpid(), pid);
		for (int i = 0; i < 3; ++i) {
			sleep(1);
			printf("A %d\n", i);
		}
		Wait(pid);
	} else {
		printf("[%d] The parent is %d\n", getpid(), getppid());
		for (int i = 0; i < 3; ++i) {
			sleep(1);
			printf("B %d\n", i);
		}
		exit(0);
	}

	printf("[%d] vfork\n", getpid());
	pid = vfork();
	HandleErr(pid);

	if (pid) {
		printf("[%d] The child is %d\n", getpid(), pid);
		for (int i = 0; i < 3; ++i) {
			sleep(1);
			printf("A %d\n", i);
		}
		Wait(pid);
	} else {
		printf("[%d] The parent is %d\n", getpid(), getppid());
		for (int i = 0; i < 3; ++i) {
			sleep(1);
			printf("B %d\n", i);
		}
		exit(0);
	}

	printf("[%d] vfork\n", getpid());
	pid = vfork();
	HandleErr(pid);

	if (pid) {
		printf("[%d] the child is %d\n", getpid(), pid);
		Wait(pid);
		for (int i = 0; i < 3; ++i) {
			sleep(1);
			printf("A %d\n", i);
		}
	} else {
		printf("[%d] the parent is %d\n", getpid(), getppid());
		for (int i = 0; i < 3; ++i) {
			sleep(1);
			printf("B %d\n", i);
		}
		execl("/bin/example/echo.exe", "the quick brown", "fox", NULL);
		Exit();
	}
	return 0;
}
