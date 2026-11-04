#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_positional_io(const char *path)
{
	char bytes[16] = { 0 };
	int fd;
	int append_fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(write(fd, "0123456789", 10) == 10);
	assert(lseek(fd, 3, SEEK_SET) == 3);

	assert(pwrite(fd, "XY", 2, 7) == 2);
	assert(lseek(fd, 0, SEEK_CUR) == 3);
	/* EOF returns the available suffix without changing the shared offset. */
	assert(pread(fd, bytes, 8, 6) == 4);
	assert(!memcmp(bytes, "6XY9", 4));
	assert(lseek(fd, 0, SEEK_CUR) == 3);

	append_fd = open(path, O_WRONLY | O_APPEND);
	assert(append_fd >= 0);
	assert(lseek(append_fd, 1, SEEK_SET) == 1);
	/* pwrite() uses its argument even when ordinary writes are append-only. */
	assert(pwrite(append_fd, "A", 1, 1) == 1);
	assert(lseek(append_fd, 0, SEEK_CUR) == 1);
	assert(write(append_fd, "B", 1) == 1);
	assert(close(append_fd) == 0);

	errno = 0;
	assert(pread(fd, bytes, 1, -1) == -1);
	assert(errno == EINVAL);
	assert(lseek(fd, 0, SEEK_CUR) == 3);

	/* A nonzero high i386 offset must not wrap around to the first byte. */
	assert(pread(fd, bytes, 1, (off_t)1 << 32) == 0);
	assert(lseek(fd, 0, SEEK_CUR) == 3);
	errno = 0;
	assert(pwrite(fd, "Z", 1, (off_t)1 << 32) == -1);
	assert(errno == EFBIG);
	assert(lseek(fd, 0, SEEK_CUR) == 3);

	assert(pread(fd, bytes, sizeof(bytes), 0) == 11);
	assert(!memcmp(bytes, "0A23456XY9B", 11));
	assert(close(fd) == 0);
	assert(unlink(path) == 0);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	test_positional_io(argv[1]);
	printf("pread-pwrite: %s\n", argv[1]);
	return 0;
}
