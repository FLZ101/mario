#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>

#include <app/util.h>

#define N 8192

unsigned char buf[N];

void test_zero()
{
	puts("test /dev/zero");

	int fd = open("/dev/zero", O_RDONLY);
	HandleErr(fd);

	for (int i = 0; i < N; ++i)
		buf[i] = i + 1;

	int n = read(fd, buf, N);
	HandleErr(n);

	assert(n == N);

	for (int i = 0; i < N; ++i)
		assert(0 == buf[i]);

	HandleErr(close(fd));
	puts("ok");
}

void test_null()
{
	puts("test /dev/null");

	int fd = open("/dev/null", O_WRONLY);
	HandleErr(fd);

	int n = write(fd, buf, N);
	HandleErr(n);

	assert(n == N);

	HandleErr(close(fd));
	puts("ok");
}

void test_full()
{
	puts("test /dev/full");

	int fd = open("/dev/full", O_WRONLY);
	HandleErr(fd);

	int n = write(fd, buf, N);
	if (n != -1 || errno != ENOSPC)
		Exit();

	HandleErr(close(fd));
	puts("ok");
}

void test_rand()
{
	puts("test /dev/urandom");

	int fd = open("/dev/urandom", O_RDONLY);
	HandleErr(fd);

	for (int i = 0; i < N; ++i)
		buf[i] = 0;

	int n = read(fd, buf, N);
	HandleErr(n);

	assert(n == N);

	for (int i = 0; i < 16; ++i)
		printf("%d", buf[i]);
	puts("");

	HandleErr(close(fd));
	puts("ok");
}

int main(int argc, char *argv[])
{
	test_zero();
	test_null();
	test_full();
	test_rand();
	return 0;
}
