#include <stdio.h>

int main(int argc, char *argv[], char *envp[])
{
	int i;

	for (i = 0; i < argc; i++)
		puts(argv[i]);
	for (i = 0; envp[i]; i++)
		puts(envp[i]);
	return 0;
}
