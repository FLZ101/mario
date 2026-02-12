#include <app/util.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char *argv[], char *envp[])
{
	cat("/etc/welcome.txt");

	while (1) {
		printf("?");
		pause();
	}
	return 0;
}
