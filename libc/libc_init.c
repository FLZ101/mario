
#include <stdlib.h>

extern int main(int, char **, char **);

void __libc_init(unsigned long *xarg)
{
	int argc;
	char **argv, **envp;

	argc = (int)xarg[0];
	argv = (char **)(xarg + 1);
	envp = argv + argc + 1;
	exit(main(argc, argv, envp));
}
