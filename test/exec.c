#include "test.h"

void test_exec(void *arg)
{
	char *argv[] = { "apple", NULL };
	char *envp[] = { "pear", NULL };

	execve("/bin/test_exec.exe", argv, envp);
}
