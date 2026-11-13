#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static void write_file(const char *path, size_t bytes)
{
	static const char payload[2048];
	int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	assert(descriptor >= 0);
	assert(write(descriptor, payload, bytes) == (ssize_t)bytes);
	assert(close(descriptor) == 0);
}

static fsblkcnt_t free_blocks(void)
{
	struct statvfs status;

	assert(statvfs("/tmp/mariofs-reclaim", &status) == 0);
	assert(status.f_bsize == 512);
	return status.f_bfree;
}

int main(void)
{
	const char *source = "/tmp/mariofs-reclaim/source";
	const char *target = "/tmp/mariofs-reclaim/target";
	fsblkcnt_t baseline;

	assert(mkdir("/tmp/mariofs-reclaim", 0700) == 0);
	write_file(source, 0);
	write_file(target, 0);
	baseline = free_blocks();
	for (unsigned int round = 0; round < 256; round++) {
		write_file(source, 1530);
		assert(free_blocks() + 4 == baseline);
		assert(truncate(source, 508) == 0);
		assert(free_blocks() + 1 == baseline);
		assert(truncate(source, 0) == 0);
		assert(free_blocks() == baseline);
	}
	write_file(target, 1530);
	assert(free_blocks() + 4 == baseline);
	for (unsigned int round = 0; round < 2048; round++) {
		write_file(source, 1530);
		assert(free_blocks() + 8 == baseline);
		assert(rename(source, target) == 0);
		assert(free_blocks() + 4 == baseline);
	}
	assert(unlink(target) == 0);
	assert(free_blocks() == baseline);
	assert(rmdir("/tmp/mariofs-reclaim") == 0);
	puts("mariofs: truncate and replacement return every freed block");
	return 0;
}
