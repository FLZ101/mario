#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdio.h>

// musl libc's sbrk always return -1, except for `sbrk(0)`.
//
// Avoid using `brk()` and `sbrk()`: the `malloc(3)` memory allocation package is the
// portable and comfortable way of allocating memory.
//
void test_sbrk()
{
	puts("------ test_sbrk");

	for (int i = 0; i < 3; ++i) {
		void *addr = sbrk(0x0800);
		printf("%x ", addr);
	}
	puts("");

	for (int i = 0; i < 3; ++i) {
		void *addr = sbrk(-0x0800);
		printf("%x ", addr);
	}
	puts("");
}

void test_linked_list();

static void write_file(const char *path, const void *buf, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	assert(fd >= 0);
	assert(write(fd, buf, len) == (ssize_t)len);
	close(fd);
}

static void test_mmap_private(void)
{
	const char *path = "/root/mm.private";
	char readbuf[4096];
	char *map;
	int fd;

	puts("------ test_mmap_private");

	write_file(path, "0123456789", 10);
	fd = open(path, O_RDWR);
	assert(fd >= 0);

	map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	assert(map != MAP_FAILED);
	assert(memcmp(map, "0123456789", 10) == 0);
	assert(map[10] == 0 && map[4095] == 0);

	/* Private writes must not leak back to the file. */
	map[0] = 'X';
	assert(msync(map, 4096, MS_SYNC) == 0);
	assert(munmap(map, 4096) == 0);
	close(fd);

	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, readbuf, sizeof(readbuf)) == 10);
	assert(memcmp(readbuf, "0123456789", 10) == 0);
	close(fd);
	printf("private-ok\n");
}

static void test_mmap_shared(void)
{
	const char *path = "/root/mm.shared";
	char readbuf[4096];
	char *map;
	int fd;

	puts("------ test_mmap_shared");

	write_file(path, "0123456789", 10);
	fd = open(path, O_RDWR);
	assert(fd >= 0);

	map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(map != MAP_FAILED);

	/* Closing the original descriptor must not invalidate the mapping. */
	close(fd);
	map[0] = 'S';
	map[1] = 'H';
	assert(msync(map, 4096, MS_SYNC) == 0);
	assert(munmap(map, 4096) == 0);

	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, readbuf, sizeof(readbuf)) == sizeof(readbuf));
	assert(memcmp(readbuf, "SH23456789", 10) == 0);
	close(fd);
	printf("shared-ok\n");
}

static void test_mmap_past_eof(void)
{
	const char *path = "/root/mm.pasteof";
	struct stat st;
	char *map;
	int fd;

	puts("------ test_mmap_past_eof");

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, 5000) == 0);
	close(fd);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(map != MAP_FAILED);
	close(fd);

	/* Pages beyond EOF fault in as zeroes. */
	assert(map[5000] == 0 && map[8191] == 0);
	map[7000] = 'E';
	assert(msync(map, 8192, MS_SYNC) == 0);
	assert(munmap(map, 8192) == 0);

	assert(stat(path, &st) == 0);
	assert(st.st_size >= 7001);
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	assert(lseek(fd, 7000, SEEK_SET) == 7000);
	{
		char byte = 0;
		assert(read(fd, &byte, 1) == 1);
		assert(byte == 'E');
	}
	close(fd);
	printf("past-eof-ok\n");
}

static void test_mmap_partial_unmap(void)
{
	const char *path = "/root/mm.partial";
	struct stat st;
	char *map;
	int fd;

	puts("------ test_mmap_partial_unmap");

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, 8192) == 0);
	close(fd);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(map != MAP_FAILED);
	close(fd);

	map[0] = 'A';
	map[4096] = 'B';
	assert(msync(map, 8192, MS_SYNC) == 0);

	/* Unmap only the first page and keep the second mapping live. */
	assert(munmap(map, 4096) == 0);
	assert(map[4096] == 'B');
	assert(munmap(map + 4096, 4096) == 0);

	assert(stat(path, &st) == 0);
	assert(st.st_size == 8192);
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	{
		char bytes[8192];
		assert(read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
		assert(bytes[0] == 'A');
		assert(bytes[4096] == 'B');
	}
	close(fd);
	printf("partial-unmap-ok\n");
}

static void test_mmap_truncate(void)
{
	const char *path = "/root/mm.truncate";
	struct stat st;
	char *map;
	int fd;

	puts("------ test_mmap_truncate");

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, 8192) == 0);
	close(fd);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(map != MAP_FAILED);
	map[4096] = 'T';

	/* Leave the page dirty.  Truncate must prevent the later munmap
	 * writeback from resurrecting bytes past the new EOF. */
	assert(ftruncate(fd, 16) == 0);
	assert(munmap(map, 8192) == 0);
	close(fd);

	assert(stat(path, &st) == 0);
	assert(st.st_size == 16);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(map != MAP_FAILED);
	assert(map[16] == 0 && map[4096] == 0 && map[8191] == 0);
	assert(munmap(map, 8192) == 0);
	close(fd);
	printf("truncate-ok\n");
}

static void test_mmap_fork_cow(void)
{
	const char *path = "/root/mm.fork";
	char *map;
	pid_t pid;
	int status;
	int fd;

	puts("------ test_mmap_fork_cow");

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, 4096) == 0);
	close(fd);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	assert(map != MAP_FAILED);
	close(fd);

	map[0] = 'P';
	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		/* The child must copy-on-write and not modify the parent page. */
		map[0] = 'C';
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(map[0] == 'P');
	assert(munmap(map, 4096) == 0);
	printf("fork-cow-ok\n");
}

static void test_mmap_shared_coherence(void)
{
	const char *path = "/root/mm.coherent";
	char *first;
	char *second;
	int fd;

	puts("------ test_mmap_shared_coherence");

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, 4096) == 0);
	close(fd);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	first = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	second = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(first != MAP_FAILED && second != MAP_FAILED);
	close(fd);

	/* Independent mappings must share the cached page. */
	first[0] = '1';
	assert(second[0] == '1');
	second[1] = '2';
	assert(first[1] == '2');
	assert(msync(first, 4096, MS_SYNC) == 0);
	assert(munmap(first, 4096) == 0);
	assert(munmap(second, 4096) == 0);
	printf("shared-coherent-ok\n");
}

static void test_mmap_shared_fork(void)
{
	const char *path = "/root/mm.sharedfork";
	char readbuf[4096];
	char *map;
	pid_t pid;
	int status;
	int fd;

	puts("------ test_mmap_shared_fork");

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, 4096) == 0);
	close(fd);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(map != MAP_FAILED);
	close(fd);

	map[0] = 'F';
	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		assert(map[0] == 'F');
		map[0] = 'C';
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/* Child exit must flush the shared mapping even while the parent's
	 * mapping is still live. */
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, readbuf, sizeof(readbuf)) == sizeof(readbuf));
	assert(readbuf[0] == 'C');
	close(fd);

	assert(map[0] == 'C');
	assert(msync(map, 4096, MS_SYNC) == 0);
	assert(munmap(map, 4096) == 0);
	printf("shared-fork-ok\n");
}

int main(int argc, char *argv[], char *envp[])
{
	test_sbrk();
	test_linked_list();
	test_mmap_private();
	test_mmap_shared();
	test_mmap_past_eof();
	test_mmap_partial_unmap();
	test_mmap_truncate();
	test_mmap_fork_cow();
	test_mmap_shared_coherence();
	test_mmap_shared_fork();
	return 0;
}
