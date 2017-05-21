#include "test.h"

void test_fork(void *arg)
{
	char *argv[] = { "apple", NULL };
	char *envp[] = { "pear", NULL };

	execve("/bin/test_fork.exe", argv, envp);
}
