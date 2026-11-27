#include <assert.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	pid_t child;
	int status;

	/* Exec starts with a dumpable mm; sftp-server drops it with PR_SET_DUMPABLE. */
	assert(prctl(PR_GET_DUMPABLE) == 1);
	assert(prctl(PR_SET_DUMPABLE, 0) == 0);
	assert(prctl(PR_GET_DUMPABLE) == 0);

	errno = 0;
	assert(prctl(PR_SET_DUMPABLE, 2) == -1);
	assert(errno == EINVAL);
	assert(prctl(PR_GET_DUMPABLE) == 0);

	/* Fork inherits mm->dumpable; a fresh exec resets it to 1. */
	child = fork();
	assert(child >= 0);
	if (child == 0) {
		assert(prctl(PR_GET_DUMPABLE) == 0);
		assert(prctl(PR_SET_DUMPABLE, 1) == 0);
		assert(prctl(PR_GET_DUMPABLE) == 1);
		_exit(0);
	}
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(prctl(PR_GET_DUMPABLE) == 0);
	assert(prctl(PR_SET_DUMPABLE, 1) == 0);
	assert(prctl(PR_GET_DUMPABLE) == 1);
	return 0;
}
