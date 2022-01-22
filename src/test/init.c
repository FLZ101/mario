#include "test.h"

void test_init(void *arg)
{
	char *argv[] = { NULL };
	char *envp[] = { NULL };

	execve("/bin/init.exe", argv, envp);
}
