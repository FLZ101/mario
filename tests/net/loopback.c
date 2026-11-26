#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "loopback:%d: %s (errno %d)\n", __LINE__, #condition, errno); \
		_exit(1); \
	} \
} while (0)

static struct sockaddr_in loop_address(void)
{
	return (struct sockaddr_in) {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
}

static void check_endpoints(int fd, int peer, uint16_t port)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);
	int result = peer ? getpeername(fd, (struct sockaddr *)&address, &length) :
		getsockname(fd, (struct sockaddr *)&address, &length);

	CHECK(result == 0 && length == sizeof(address));
	CHECK(address.sin_family == AF_INET && address.sin_port == port);
	CHECK(address.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
}

static void read_exact(int fd, void *buffer, size_t length)
{
	char *target = buffer;

	while (length) {
		ssize_t count = read(fd, target, length);
		CHECK(count > 0);
		target += count;
		length -= count;
	}
}

static void write_exact(int fd, const void *buffer, size_t length)
{
	const char *source = buffer;

	while (length) {
		ssize_t count = write(fd, source, length);
		CHECK(count > 0);
		source += count;
		length -= count;
	}
}

static void tcp_loopback(void)
{
	struct sockaddr_in address = { .sin_family = AF_INET };
	struct sockaddr_in peer;
	socklen_t length = sizeof(address);
	char payload[4096];
	char buffer[sizeof(payload)];
	int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int other;
	int connection;
	int status;
	pid_t child;

	for (size_t index = 0; index < sizeof(payload); index++)
		payload[index] = index % 251;
	CHECK(listener >= 0);
	CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	CHECK(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
	CHECK(length == sizeof(address) && address.sin_family == AF_INET);
	CHECK(address.sin_port && address.sin_addr.s_addr == htonl(INADDR_ANY));
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	other = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(other >= 0);
	CHECK(bind(other, (struct sockaddr *)&address, sizeof(address)) == -1);
	CHECK(errno == EADDRINUSE);
	CHECK(connect(other, (struct sockaddr *)&address, sizeof(address)) == -1);
	CHECK(errno == ECONNREFUSED);
	CHECK(close(other) == 0);
	CHECK(listen(listener, 2) == 0);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(close(listener) == 0);
		usleep(100000);
		connection = socket(AF_INET, SOCK_STREAM, 0);
		CHECK(connection >= 0);
		CHECK(connect(connection, (struct sockaddr *)&address, sizeof(address)) == 0);
		check_endpoints(connection, 1, address.sin_port);
		write_exact(connection, payload, sizeof(payload));
		CHECK(shutdown(connection, SHUT_WR) == 0);
		read_exact(connection, buffer, 4);
		CHECK(!memcmp(buffer, "pong", 4));
		CHECK(read(connection, buffer, 1) == 0);
		CHECK(close(connection) == 0);
		_exit(0);
	}
	length = sizeof(peer);
	connection = accept4(listener, (struct sockaddr *)&peer, &length, SOCK_CLOEXEC);
	CHECK(connection >= 0 && length == sizeof(peer) && peer.sin_family == AF_INET);
	CHECK(fcntl(connection, F_GETFD) == FD_CLOEXEC);
	CHECK(peer.sin_port && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
	check_endpoints(connection, 0, address.sin_port);
	check_endpoints(connection, 1, peer.sin_port);
	read_exact(connection, buffer, sizeof(buffer));
	CHECK(!memcmp(buffer, payload, sizeof(buffer)));
	CHECK(read(connection, buffer, 1) == 0);
	CHECK(write(connection, "pong", 4) == 4);
	CHECK(shutdown(connection, SHUT_WR) == 0);
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(close(connection) == 0 && close(listener) == 0);
}

static void udp_loopback(void)
{
	struct sockaddr_in address = loop_address();
	struct sockaddr_in source;
	socklen_t length = sizeof(address);
	int receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	int sender = socket(AF_INET, SOCK_DGRAM, 0);
	char buffer[1473] = { 0 };

	CHECK(receiver >= 0 && sender >= 0);
	CHECK(bind(receiver, (struct sockaddr *)&address, sizeof(address) - 1) == -1);
	CHECK(errno == EINVAL);
	CHECK(syscall(SYS_bind, receiver, 1UL, sizeof(address)) == -1 && errno == EFAULT);
	CHECK(bind(receiver, (struct sockaddr *)&address, sizeof(address)) == 0);
	CHECK(getsockname(receiver, (struct sockaddr *)&address, &length) == 0);
	CHECK(length == sizeof(address) && address.sin_port);
	CHECK(sendto(sender, buffer, 1473, 0, (struct sockaddr *)&address, length) == -1);
	CHECK(errno == EMSGSIZE);
	CHECK(sendto(sender, "hello", 5, 0, (struct sockaddr *)&address, length) == 5);
	CHECK(connect(sender, (struct sockaddr *)&address, length) == 0);
	check_endpoints(sender, 1, address.sin_port);
	CHECK(send(sender, "", 0, 0) == 0);
	CHECK(send(sender, "last", 4, 0) == 4);
	CHECK(syscall(SYS_recvfrom, receiver, 1UL, 1, 0, 0, 0) == -1 && errno == EFAULT);
	length = sizeof(source);
	CHECK(recvfrom(receiver, buffer, 2, 0, (struct sockaddr *)&source, &length) == 2);
	CHECK(!memcmp(buffer, "he", 2));
	CHECK(length == sizeof(source) && source.sin_family == AF_INET);
	CHECK(source.sin_port && source.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
	check_endpoints(sender, 0, source.sin_port);
	CHECK(recv(receiver, buffer, sizeof(buffer), 0) == 0);
	CHECK(recv(receiver, buffer, 2, MSG_TRUNC) == 4 && !memcmp(buffer, "la", 2));
	memset(buffer, 0x42, 1472);
	CHECK(send(sender, buffer, 1472, 0) == 1472);
	memset(buffer, 0, sizeof(buffer));
	CHECK(recv(receiver, buffer, sizeof(buffer), 0) == 1472);
	for (size_t index = 0; index < 1472; index++)
		CHECK(buffer[index] == 0x42);
	CHECK(close(sender) == 0 && close(receiver) == 0);
}

static long raw_socket(int legacy, long direct, unsigned long call,
		       unsigned long first, unsigned long second, unsigned long third,
		       unsigned long fourth, unsigned long fifth, unsigned long sixth)
{
	unsigned long args[] = { first, second, third, fourth, fifth, sixth };

	return legacy ? syscall(SYS_socketcall, call, args) :
		syscall(direct, first, second, third, fourth, fifth, sixth);
}

static void raw_udp_loopback(int legacy)
{
	struct sockaddr_in address = loop_address();
	struct sockaddr_in peer;
	socklen_t length = sizeof(address);
	char buffer[4];
	int receiver = raw_socket(legacy, SYS_socket, 1, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
	int sender = raw_socket(legacy, SYS_socket, 1, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);

	CHECK(receiver >= 0 && sender >= 0);
	CHECK(raw_socket(legacy, SYS_bind, 2, receiver, (unsigned long)&address,
			 sizeof(address), 0, 0, 0) == 0);
	CHECK(raw_socket(legacy, SYS_getsockname, 6, receiver, (unsigned long)&address,
			 (unsigned long)&length, 0, 0, 0) == 0);
	CHECK(address.sin_port && length == sizeof(address));
	CHECK(raw_socket(legacy, SYS_connect, 3, sender, (unsigned long)&address,
			 length, 0, 0, 0) == 0);
	length = sizeof(peer);
	CHECK(raw_socket(legacy, SYS_getpeername, 7, sender, (unsigned long)&peer,
			 (unsigned long)&length, 0, 0, 0) == 0);
	CHECK(length == sizeof(peer) && !memcmp(&peer, &address, length));
	CHECK(raw_socket(legacy, SYS_sendto, 11, sender, (unsigned long)"data", 4, 0,
			 (unsigned long)&address, length) == 4);
	length = sizeof(peer);
	CHECK(raw_socket(legacy, SYS_recvfrom, 12, receiver, (unsigned long)buffer, 4,
			 0, (unsigned long)&peer, (unsigned long)&length) == 4);
	CHECK(!memcmp(buffer, "data", 4) && length == sizeof(peer));
	CHECK(peer.sin_family == AF_INET && peer.sin_port);
	CHECK(close(sender) == 0 && close(receiver) == 0);
}

static void raw_tcp_loopback(int legacy)
{
	struct sockaddr_in address = loop_address();
	socklen_t length = sizeof(address);
	char buffer[4];
	int listener = raw_socket(legacy, SYS_socket, 1, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
	int sender = raw_socket(legacy, SYS_socket, 1, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
	int receiver;

	CHECK(listener >= 0 && sender >= 0);
	CHECK(raw_socket(legacy, SYS_bind, 2, listener, (unsigned long)&address,
			 sizeof(address), 0, 0, 0) == 0);
	CHECK(raw_socket(legacy, SYS_getsockname, 6, listener, (unsigned long)&address,
			 (unsigned long)&length, 0, 0, 0) == 0);
	CHECK(raw_socket(legacy, SYS_listen, 4, listener, 1, 0, 0, 0, 0) == 0);
	CHECK(raw_socket(legacy, SYS_connect, 3, sender, (unsigned long)&address,
			 length, 0, 0, 0) == 0);
	receiver = raw_socket(legacy, SYS_accept4, 18, listener, 0, 0, SOCK_CLOEXEC, 0, 0);
	CHECK(receiver >= 0 && fcntl(receiver, F_GETFD) == FD_CLOEXEC);
	CHECK(raw_socket(legacy, SYS_sendto, 9, sender, (unsigned long)"data", 4,
			 0, 0, 0) == 4);
	CHECK(raw_socket(legacy, SYS_shutdown, 13, sender, SHUT_WR, 0, 0, 0, 0) == 0);
	if (legacy)
		CHECK(raw_socket(1, SYS_recvfrom, 10, receiver, (unsigned long)buffer,
				 sizeof(buffer), 0, 0, 0) == sizeof(buffer));
	else
		read_exact(receiver, buffer, sizeof(buffer));
	CHECK(!memcmp(buffer, "data", 4) && read(receiver, buffer, 1) == 0);
	CHECK(close(sender) == 0 && close(receiver) == 0 && close(listener) == 0);
}

static void discard_passed_socket(void)
{
	struct sockaddr_in address = loop_address();
	socklen_t length = sizeof(address);
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	char data = '!';
	struct iovec iov = { .iov_base = &data, .iov_len = 1 };
	struct msghdr message = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	int pair[2];
	int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

	CHECK(socket_fd >= 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	CHECK(bind(socket_fd, (struct sockaddr *)&address, length) == 0);
	CHECK(getsockname(socket_fd, (struct sockaddr *)&address, &length) == 0);
	header->cmsg_len = CMSG_LEN(sizeof(socket_fd));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(header), &socket_fd, sizeof(socket_fd));
	CHECK(sendmsg(pair[0], &message, 0) == 1);
	CHECK(close(socket_fd) == 0);
	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(socket_fd >= 0);
	CHECK(bind(socket_fd, (struct sockaddr *)&address, length) == -1 && errno == EADDRINUSE);
	CHECK(read(pair[1], &data, 1) == 1 && data == '!');
	CHECK(bind(socket_fd, (struct sockaddr *)&address, length) == 0);
	CHECK(close(socket_fd) == 0 && close(pair[0]) == 0 && close(pair[1]) == 0);
}

int main(void)
{
	CHECK(socket(AF_INET, SOCK_RAW, 0) == -1 && errno == ESOCKTNOSUPPORT);
	CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 && errno == EPROTONOSUPPORT);
	CHECK(socket(AF_INET6, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT);
	tcp_loopback();
	udp_loopback();
	raw_tcp_loopback(0);
	raw_tcp_loopback(1);
	raw_udp_loopback(0);
	raw_udp_loopback(1);
	discard_passed_socket();
	puts("NET_LOOPBACK_OK");
	return 0;
}
