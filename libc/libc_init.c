
#include <stdlib.h>

extern int main(int, char **, char **);

extern char **environ;

void __libc_init(unsigned long *xarg)
{
	int argc;
	char **argv, **envp;

	argc = (int)xarg[0];
	argv = (char **)(xarg + 1);
	envp = argv + argc + 1;

	environ = envp;
	exit(main(argc, argv, envp));
}
