#include "test.h"

void test_signal(void *arg)
{
	char *argv[] = { "apple", NULL };
	char *envp[] = { "pear", NULL };

	execve("/bin/test_signal.exe", argv, envp);
}
