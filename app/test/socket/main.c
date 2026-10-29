#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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
	assert(close(fd[1]) == 0);
	assert(read(fd[0], buf, 1) == 0);
	assert(write(fd[0], "x", 1) == -1);
	assert(errno == EPIPE);
	assert(close(fd[0]) == 0);
	return 0;
}
