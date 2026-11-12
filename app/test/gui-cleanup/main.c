#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static rlim_t descriptor_limit(void)
{
	struct rlimit limit;

	assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
	assert(limit.rlim_cur > 3 && limit.rlim_cur <= 4096);
	return limit.rlim_cur;
}

static void count_available_ptys(void)
{
	rlim_t limit = descriptor_limit();
	int *masters = calloc(limit, sizeof(*masters));
	size_t count = 0;
	int master;

	assert(masters != NULL);
	while ((master = open("/dev/ptmx", O_RDWR | O_NOCTTY)) >= 0) {
		assert(count < limit);
		masters[count++] = master;
	}
	assert(errno == ENOSPC);
	for (size_t index = 0; index < count; index++)
		assert(close(masters[index]) == 0);
	free(masters);
	printf("%zu\n", count);
}

static void audit_child(void)
{
	struct stat slave;
	rlim_t limit = descriptor_limit();
	FILE *marker;

	assert(fstat(STDIN_FILENO, &slave) == 0 && S_ISCHR(slave.st_mode));
	assert(isatty(STDIN_FILENO));
	for (int descriptor = STDOUT_FILENO; descriptor <= STDERR_FILENO; descriptor++) {
		struct stat stream;

		assert(fstat(descriptor, &stream) == 0 && S_ISCHR(stream.st_mode));
		assert(stream.st_rdev == slave.st_rdev);
	}
	for (rlim_t descriptor = 3; descriptor < limit; descriptor++) {
		int flags;

		errno = 0;
		flags = fcntl(descriptor, F_GETFD);
		if (flags >= 0) {
			struct stat inherited;

			assert(fstat(descriptor, &inherited) == 0);
			fprintf(stderr, "gui-cleanup: inherited fd %lu mode %o device %llu\n",
				(unsigned long)descriptor, (unsigned int)inherited.st_mode,
				(unsigned long long)inherited.st_rdev);
		}
		assert(flags < 0 && errno == EBADF);
	}
	marker = fopen("/tmp/gui-cleanup.child", "w");
	assert(marker != NULL);
	assert(fprintf(marker, "%d\n", getpid()) > 0);
	assert(fclose(marker) == 0);
	puts("gui-cleanup: child descriptors clean");
	fflush(stdout);
	while (access("/tmp/gui-cleanup.exit", F_OK) < 0)
		usleep(100000);
}

int main(int argc, char **argv)
{
	assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--pty-pool")));
	if (argc == 2)
		count_available_ptys();
	else
		audit_child();
	return 0;
}
