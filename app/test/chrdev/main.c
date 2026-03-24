#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>

#include <app/util.h>

#define N 8192

char buf[N];

void test_zero()
{
	puts("test /dev/zero");

	int fd = open("/dev/zero", O_RDONLY);
	if (-1 == fd)
		Exit();

	for (int i = 0; i < N; ++i)
		buf[i] = i + 1;

	int n = read(fd, buf, N);
	if (-1 == n)
		Exit();
	assert(n == N);

	for (int i = 0; i < N; ++i)
		assert(0 == buf[i]);

	puts("ok");
}

void test_null()
{
	puts("test /dev/null");

	int fd = open("/dev/null", O_WRONLY);
	if (-1 == fd)
		Exit();

	int n = write(fd, buf, N);
	if (-1 == n)
		Exit();
	assert(n == N);

	puts("ok");
}

int main(int argc, char *argv[])
{
	test_zero();
	test_null();
	return 0;
}
