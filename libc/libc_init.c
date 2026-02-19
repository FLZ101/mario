
#include <stdlib.h>

extern int main(int, char **, char **);

void __libc_init(unsigned long *xarg)
{
	int argc;
	char **argv, **envp;

	argc = (int)*(xarg++);
	argv = (char **)(*xarg++);
	envp = (char **)(*xarg);

	exit(main(argc, argv, envp));
}
