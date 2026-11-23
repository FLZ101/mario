#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int send_buffer_size(int descriptor)
{
	int size;
	socklen_t length = sizeof(size);

	assert(getsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &size, &length) == 0);
	assert(length == sizeof(size));
	return size;
}

static void test_send_buffer(void)
{
	static char data[65536], received[65536];
	int pair[2];
	int requested = sizeof(data);
	int default_size;

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
	default_size = send_buffer_size(pair[0]);
	assert(default_size == 4096);
	for (size_t index = 0; index < sizeof(data); index++)
		data[index] = index % 251;
	assert(write(pair[0], data, default_size - 2) == default_size - 2);
	assert(read(pair[1], received, default_size - 4) == default_size - 4);
	assert(write(pair[0], "wrap", 4) == 4);
	assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested)) == 0);
	assert(send_buffer_size(pair[0]) == requested);
	assert(send_buffer_size(pair[1]) == default_size);
	assert(write(pair[0], data, sizeof(data)) == sizeof(data) - 6);
	assert(write(pair[0], "x", 1) == -1 && errno == EAGAIN);
	unsigned long args[] = {pair[0], SOL_SOCKET, SO_SNDBUF,
		(unsigned long)&default_size, sizeof(default_size)};
	assert(syscall(SYS_socketcall, 14, args) == -1 && errno == EBUSY);
	assert(read(pair[1], received, sizeof(received)) == sizeof(received));
	assert(!memcmp(received, data + default_size - 4, 2));
	assert(!memcmp(received + 2, "wrap", 4));
	assert(!memcmp(received + 6, data, sizeof(data) - 6));
	assert(syscall(SYS_socketcall, 14, args) == 0);
	assert(send_buffer_size(pair[0]) == default_size);
	assert(close(pair[0]) == 0);
	assert(close(pair[1]) == 0);
}

int main(void)
{
	char buf[8];
	int fd[2];
	struct pollfd pfd;

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd) == 0);
	assert(send(fd[0], "ping", 4, 0) == 4);
	assert(recv(fd[1], buf, 4, 0) == 4);
	assert(!__builtin_memcmp(buf, "ping", 4));
	assert(read(fd[1], buf, 1) == -1);
	assert(errno == EAGAIN);
	assert(write(fd[1], "pong", 4) == 4);
	pfd.fd = fd[0];
	pfd.events = POLLIN;
	pfd.revents = 0;
	assert(poll(&pfd, 1, 0) == 1);
	assert(pfd.revents & POLLIN);
	assert(read(fd[0], buf, 4) == 4);
	assert(!__builtin_memcmp(buf, "pong", 4));
	assert(shutdown(fd[1], SHUT_WR) == 0);
	assert(read(fd[0], buf, 1) == 0);
	assert(write(fd[1], "x", 1) == -1);
	assert(errno == EPIPE);
	assert(close(fd[1]) == 0);
	assert(read(fd[0], buf, 1) == 0);
	assert(write(fd[0], "x", 1) == -1);
	assert(errno == EPIPE);
	assert(close(fd[0]) == 0);
	test_send_buffer();
	return 0;
}
