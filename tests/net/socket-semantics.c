#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "socket-semantics:%d: %s (errno %d)\n", __LINE__, #condition, errno); \
		_exit(1); \
	} \
} while (0)

static struct sockaddr_in loop_address(void)
{
	return (struct sockaddr_in) {
		.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
}

static void bind_port(int fd, struct sockaddr_in *address)
{
	socklen_t length = sizeof(*address);

	CHECK(bind(fd, (struct sockaddr *)address, length) == 0);
	CHECK(getsockname(fd, (struct sockaddr *)address, &length) == 0);
	CHECK(length == sizeof(*address) && address->sin_port);
}

static int ready(int fd, short events, int timeout)
{
	struct pollfd item = { .fd = fd, .events = events };
	int result = poll(&item, 1, timeout);

	CHECK(result >= 0);
	return item.revents;
}

static int get_option(int fd, int level, int option)
{
	int value = -1;
	socklen_t length = sizeof(value);

	CHECK(getsockopt(fd, level, option, &value, &length) == 0);
	CHECK(length == sizeof(value));
	return value;
}

static volatile sig_atomic_t pipes;

static void signal_pipe(int signal)
{
	(void)signal;
	pipes++;
}

static void tcp_semantics(void)
{
	struct sockaddr_in address = loop_address();
	struct sigaction action = { .sa_handler = signal_pipe };
	struct timeval timeout = { .tv_sec = 2 };
	fd_set readfds;
	char buffer[4];
	int enabled = 1;
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	int pending = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	int server, events;

	CHECK(listener >= 0 && client >= 0 && pending >= 0);
	CHECK(fcntl(listener, F_GETFL) & O_NONBLOCK);
	CHECK(fcntl(listener, F_GETFD) == FD_CLOEXEC);
	CHECK(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0);
	CHECK(setsockopt(listener, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0);
	CHECK(setsockopt(client, IPPROTO_TCP, TCP_FASTOPEN_CONNECT, &enabled, sizeof(enabled)) == -1 &&
	      errno == ENOPROTOOPT);
	CHECK(get_option(listener, SOL_SOCKET, SO_TYPE) == SOCK_STREAM);
	bind_port(listener, &address);
	CHECK(listen(listener, 1) == 0);
	CHECK(accept(listener, NULL, NULL) == -1 && errno == EAGAIN);
	CHECK(ready(listener, POLLIN | POLLOUT, 0) == 0);
	CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == -1 &&
	      errno == EINPROGRESS);
	CHECK(ready(client, POLLOUT, 2000) == POLLOUT);
	CHECK(get_option(client, SOL_SOCKET, SO_ERROR) == 0);
	CHECK(ready(listener, POLLIN, 2000) == POLLIN);
	CHECK(connect(pending, (struct sockaddr *)&address, sizeof(address)) == -1 &&
	      errno == EINPROGRESS);
	CHECK(connect(pending, (struct sockaddr *)&address, sizeof(address)) == -1 &&
	      errno == EALREADY);
	CHECK(ready(pending, POLLOUT, 0) == 0);
	CHECK(close(pending) == 0);
	CHECK(accept4(listener, NULL, NULL, 1) == -1 && errno == EINVAL);
	server = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	CHECK(server >= 0 && fcntl(server, F_GETFL) & O_NONBLOCK);
	CHECK(fcntl(server, F_GETFD) == FD_CLOEXEC);
	CHECK(get_option(server, SOL_SOCKET, SO_REUSEADDR) == 1);
	CHECK(get_option(server, IPPROTO_TCP, TCP_NODELAY) == 1);
	CHECK(syscall(SYS_socketcall, 5, (unsigned long[]){ listener, 0, 0 }) == -1 && errno == EAGAIN);
	CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == -1 && errno == EISCONN);
	CHECK(recv(server, buffer, sizeof(buffer), 0) == -1 && errno == EAGAIN);
	CHECK(write(client, "abc", 3) == 3);
	CHECK(shutdown(client, SHUT_WR) == 0);
	FD_ZERO(&readfds);
	FD_SET(server, &readfds);
	CHECK(select(server + 1, &readfds, NULL, NULL, &timeout) == 1);
	CHECK(FD_ISSET(server, &readfds));
	CHECK(recv(server, buffer, sizeof(buffer), MSG_PEEK) == 3 && !memcmp(buffer, "abc", 3));
	CHECK(read(server, buffer, 1) == 1 && buffer[0] == 'a');
	CHECK(read(server, buffer, sizeof(buffer)) == 2 && !memcmp(buffer, "bc", 2));
	events = ready(server, POLLIN | POLLOUT | POLLRDHUP, 2000);
	CHECK(events == (POLLIN | POLLOUT | POLLRDHUP));
	CHECK(read(server, buffer, sizeof(buffer)) == 0);
	CHECK(write(server, "ok", 2) == 2);
	CHECK(ready(client, POLLIN, 2000) == POLLIN);
	CHECK(read(client, buffer, sizeof(buffer)) == 2 && !memcmp(buffer, "ok", 2));
	CHECK(sigaction(SIGPIPE, &action, NULL) == 0);
	CHECK(send(client, "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE && pipes == 0);
	CHECK(write(client, "x", 1) == -1 && errno == EPIPE && pipes == 1);
	CHECK(close(server) == 0 && close(client) == 0 && close(listener) == 0);

	address = loop_address();
	listener = socket(AF_INET, SOCK_STREAM, 0);
	client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	CHECK(listener >= 0 && client >= 0);
	bind_port(listener, &address);
	CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == -1 &&
	      errno == EINPROGRESS);
	events = ready(client, POLLOUT, 2000);
	CHECK((events & (POLLOUT | POLLERR)) == (POLLOUT | POLLERR));
	CHECK(syscall(SYS_getsockopt, client, SOL_SOCKET, SO_ERROR, 1UL, &((socklen_t){4})) == -1 &&
	      errno == EFAULT);
	CHECK(get_option(client, SOL_SOCKET, SO_ERROR) == ECONNREFUSED);
	CHECK(get_option(client, SOL_SOCKET, SO_ERROR) == 0);
	CHECK(!(ready(client, POLLOUT, 0) & POLLERR));
	CHECK(close(client) == 0 && close(listener) == 0);
}

static long raw_message(int legacy, int fd, struct msghdr *message, int flags, int send)
{
	unsigned long args[] = { fd, (unsigned long)message, flags };

	return legacy ? syscall(SYS_socketcall, send ? 16 : 17, args) :
		syscall(send ? SYS_sendmsg : SYS_recvmsg, fd, message, flags);
}

static unsigned int udp_drops(void)
{
	unsigned int drops;
	FILE *file = fopen("/sys/kernel/debug/net", "r");

	CHECK(file);
	CHECK(fscanf(file, "udp_rx_drops %u", &drops) == 1 && fclose(file) == 0);
	return drops;
}

static void udp_messages(int legacy)
{
	struct sockaddr_in address = loop_address(), source;
	char buffer[4];
	struct iovec tx[] = { { "abc", 3 }, { "def", 3 } };
	struct iovec rx[] = { { buffer, 1 }, { buffer + 1, 3 } };
	struct msghdr message = {
		.msg_name = &address, .msg_namelen = sizeof(address),
		.msg_iov = tx, .msg_iovlen = 2,
	};
	unsigned int drops = udp_drops();
	int sender = socket(AF_INET, SOCK_DGRAM, 0);
	int receiver = socket(AF_INET, SOCK_DGRAM, 0);

	CHECK(sender >= 0 && receiver >= 0);
	bind_port(receiver, &address);
	CHECK(recv(receiver, buffer, sizeof(buffer), MSG_DONTWAIT) == -1 && errno == EAGAIN);
	message.msg_iovlen = 1025;
	CHECK(raw_message(legacy, sender, &message, 0, 1) == -1 && errno == EINVAL);
	message.msg_iovlen = 2;
	message.msg_controllen = 16;
	CHECK(raw_message(legacy, sender, &message, 0, 1) == -1 && errno == EOPNOTSUPP);
	message.msg_controllen = 0;
	CHECK(raw_message(legacy, sender, &message, 0, 1) == 6);
	CHECK(raw_message(legacy, sender, &message, 0, 1) == 6);
	message = (struct msghdr) {
		.msg_name = &source, .msg_namelen = sizeof(source),
		.msg_iov = rx, .msg_iovlen = 2, .msg_flags = -1,
	};
	CHECK(raw_message(legacy, receiver, &message, 0, 0) == 4);
	CHECK(message.msg_flags == MSG_TRUNC && !memcmp(buffer, "abcd", 4));
	CHECK(message.msg_namelen == sizeof(source) && source.sin_family == AF_INET && source.sin_port);
	CHECK(raw_message(legacy, receiver, &message, MSG_TRUNC, 0) == 6);
	CHECK(message.msg_flags == MSG_TRUNC && !memcmp(buffer, "abcd", 4));
	CHECK(connect(sender, (struct sockaddr *)&address, sizeof(address)) == 0);
	CHECK(writev(sender, tx, 2) == 6);
	CHECK(readv(receiver, rx, 2) == 4 && !memcmp(buffer, "abcd", 4));
	CHECK(connect(sender, &((struct sockaddr){.sa_family = AF_UNSPEC}), sizeof(sa_family_t)) == 0);
	CHECK(send(sender, "x", 1, 0) == -1 && errno == EDESTADDRREQ);
	for (char value = '0'; value <= '8'; value++)
		CHECK(sendto(sender, &value, 1, 0, (struct sockaddr *)&address, sizeof(address)) == 1);
	for (char value = '0'; value <= '7'; value++)
		CHECK(recv(receiver, buffer, sizeof(buffer), 0) == 1 && buffer[0] == value);
	CHECK(recv(receiver, buffer, sizeof(buffer), MSG_DONTWAIT) == -1 && errno == EAGAIN);
	CHECK(udp_drops() == drops + 1);
	CHECK(close(receiver) == 0 && close(sender) == 0);
}

struct waiter {
	int fd;
	pid_t tid;
	ssize_t result;
	int error;
	char byte;
};

static void *receive(void *data)
{
	struct waiter *waiter = data;

	__atomic_store_n(&waiter->tid, syscall(SYS_gettid), __ATOMIC_RELEASE);
	waiter->result = recv(waiter->fd, &waiter->byte, 1, 0);
	waiter->error = errno;
	return NULL;
}

static void wait_blocked(struct waiter *waiter)
{
	char path[48], status[256];
	pid_t tid;

	while (!(tid = __atomic_load_n(&waiter->tid, __ATOMIC_ACQUIRE)))
		usleep(10000);
	snprintf(path, sizeof(path), "/proc/%d/stat", tid);
	for (int attempt = 0; attempt < 100; attempt++) {
		int fd = open(path, O_RDONLY);
		ssize_t len;
		char *state;

		CHECK(fd >= 0);
		len = read(fd, status, sizeof(status) - 1);
		CHECK(len > 0 && close(fd) == 0);
		status[len] = 0;
		state = strrchr(status, ')');
		if (state && state[2] == 'S')
			return;
		usleep(10000);
	}
	CHECK(0);
}

static void handle_interrupt(int signal)
{
	(void)signal;
}

static void close_and_interrupt(void)
{
	struct sockaddr_in address = loop_address();
	struct sigaction action = { .sa_handler = handle_interrupt };
	struct waiter waiter = { .fd = socket(AF_INET, SOCK_DGRAM, 0) };
	int sender = socket(AF_INET, SOCK_DGRAM, 0);
	int copy, replacement;
	pthread_t thread;

	CHECK(waiter.fd >= 0 && sender >= 0);
	bind_port(waiter.fd, &address);
	replacement = fcntl(waiter.fd, F_DUPFD_CLOEXEC, 0);
	CHECK(replacement >= 0 && fcntl(replacement, F_GETFD) == FD_CLOEXEC);
	CHECK(close(replacement) == 0);
	copy = dup(waiter.fd);
	CHECK(copy >= 0 && close(waiter.fd) == 0);
	waiter.fd = copy;
	CHECK(pthread_create(&thread, NULL, receive, &waiter) == 0);
	wait_blocked(&waiter);
	CHECK(close(copy) == 0);
	replacement = dup2(sender, copy);
	CHECK(replacement == copy);
	CHECK(sendto(sender, "!", 1, 0, (struct sockaddr *)&address, sizeof(address)) == 1);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(waiter.result == 1 && waiter.byte == '!');
	CHECK(recv(replacement, &waiter.byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
	CHECK(close(replacement) == 0);
	waiter.fd = socket(AF_INET, SOCK_DGRAM, 0);
	__atomic_store_n(&waiter.tid, 0, __ATOMIC_RELEASE);
	CHECK(waiter.fd >= 0 && sigaction(SIGUSR1, &action, NULL) == 0);
	CHECK(pthread_create(&thread, NULL, receive, &waiter) == 0);
	wait_blocked(&waiter);
	CHECK(syscall(SYS_tkill, __atomic_load_n(&waiter.tid, __ATOMIC_ACQUIRE), SIGUSR1) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(waiter.result == -1 && waiter.error == EINTR);
	CHECK(close(waiter.fd) == 0 && close(sender) == 0);
}

static void io_pressure(void)
{
	struct sockaddr_in address = loop_address();
	pthread_t threads[16];
	struct waiter waiters[16] = { 0 };
	int shared = socket(AF_INET, SOCK_DGRAM, 0);
	int other = socket(AF_INET, SOCK_DGRAM, 0);
	char buffer;

	CHECK(shared >= 0 && other >= 0);
	bind_port(other, &address);
	CHECK(connect(shared, (struct sockaddr *)&address, sizeof(address)) == 0);
	for (size_t index = 0; index < 16; index++) {
		waiters[index].fd = shared;
		CHECK(pthread_create(&threads[index], NULL, receive, &waiters[index]) == 0);
		wait_blocked(&waiters[index]);
	}
	CHECK(recv(other, &buffer, 1, 0) == -1 && errno == ENOBUFS);
	CHECK(send(shared, "x", 1, 0) == 1);
	CHECK(recv(other, &buffer, 1, MSG_DONTWAIT) == 1 && buffer == 'x');
	CHECK(shutdown(shared, SHUT_RD) == 0);
	for (size_t index = 0; index < 16; index++) {
		CHECK(pthread_join(threads[index], NULL) == 0);
		CHECK(waiters[index].result == 0);
	}
	CHECK(send(shared, "y", 1, 0) == 1);
	CHECK(recv(other, &buffer, 1, 0) == 1 && buffer == 'y');
	CHECK(close(shared) == 0 && close(other) == 0);
}

static void pcb_pressure(void)
{
	struct sockaddr_in address = loop_address();
	int udp[16], listeners[8], tcp[24];
	int bound = socket(AF_INET, SOCK_STREAM, 0);
	int failed = socket(AF_INET, SOCK_STREAM, 0);

	CHECK(bound >= 0 && failed >= 0);
	bind_port(bound, &address);
	CHECK(connect(failed, (struct sockaddr *)&address, sizeof(address)) == -1 && errno == ECONNREFUSED);
	CHECK(close(bound) == 0);
	for (size_t index = 0; index < 16; index++) {
		udp[index] = socket(AF_INET, SOCK_DGRAM, 0);
		CHECK(udp[index] >= 0);
	}
	CHECK(socket(AF_INET, SOCK_DGRAM, 0) == -1 && errno == ENOBUFS);
	for (size_t index = 0; index < 8; index++) {
		listeners[index] = socket(AF_INET, SOCK_STREAM, 0);
		CHECK(listeners[index] >= 0 && listen(listeners[index], 1) == 0);
	}
	for (size_t index = 0; index < 23; index++) {
		tcp[index] = socket(AF_INET, SOCK_STREAM, 0);
		CHECK(tcp[index] >= 0);
	}
	CHECK(listen(tcp[0], 1) == -1 && errno == ENOBUFS);
	CHECK(socket(AF_INET, SOCK_STREAM, 0) == -1 && errno == ENOBUFS);
	CHECK(close(failed) == 0);
	tcp[23] = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(tcp[23] >= 0);
	CHECK(socket(AF_INET, SOCK_STREAM, 0) == -1 && errno == ENOBUFS);
	CHECK(close(listeners[0]) == 0 && listen(tcp[0], 1) == 0);
	listeners[0] = tcp[0];
	for (size_t index = 1; index < 24; index++)
		CHECK(close(tcp[index]) == 0);
	for (size_t index = 0; index < 8; index++)
		CHECK(close(listeners[index]) == 0);
	for (size_t index = 0; index < 16; index++)
		CHECK(close(udp[index]) == 0);
	CHECK((bound = socket(AF_INET, SOCK_DGRAM, 0)) >= 0 && close(bound) == 0);
}

static void tcp_backpressure(void)
{
	struct sockaddr_in address = loop_address();
	char buffer[4096];
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int sender = socket(AF_INET, SOCK_STREAM, 0);
	int receiver;
	size_t sent = 0, received = 0;
	int blocked = 0;

	CHECK(listener >= 0 && sender >= 0);
	bind_port(listener, &address);
	CHECK(listen(listener, 1) == 0);
	CHECK(connect(sender, (struct sockaddr *)&address, sizeof(address)) == 0);
	receiver = accept(listener, NULL, NULL);
	CHECK(receiver >= 0 && fcntl(sender, F_SETFL, O_NONBLOCK) == 0);
	memset(buffer, 'z', sizeof(buffer));
	for (int attempt = 0; attempt < 100; attempt++) {
		ssize_t bytes = write(sender, buffer, sizeof(buffer));

		if (bytes > 0) {
			sent += bytes;
			continue;
		}
		CHECK(bytes == -1 && errno == EAGAIN);
		if (!ready(sender, POLLOUT, 100)) {
			blocked = 1;
			break;
		}
	}
	CHECK(blocked && sent > 0);
	while (received < sent) {
		ssize_t bytes = read(receiver, buffer, sizeof(buffer));
		CHECK(bytes > 0);
		for (ssize_t index = 0; index < bytes; index++)
			CHECK(buffer[index] == 'z');
		received += bytes;
	}
	CHECK(received == sent && ready(sender, POLLOUT, 2000) == POLLOUT);
	CHECK(send(sender, "!", 1, MSG_DONTWAIT) == 1);
	CHECK(read(receiver, buffer, sizeof(buffer)) == 1 && buffer[0] == '!');
	CHECK(close(receiver) == 0 && close(sender) == 0 && close(listener) == 0);
}

static void time_wait(void)
{
	struct sockaddr_in local = loop_address(), peer = loop_address();
	struct timespec start, now;
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int client = socket(AF_INET, SOCK_STREAM, 0);
	int server, probe;
	char byte;

	CHECK(listener >= 0 && client >= 0);
	bind_port(listener, &peer);
	bind_port(client, &local);
	CHECK(listen(listener, 1) == 0);
	CHECK(connect(client, (struct sockaddr *)&peer, sizeof(peer)) == 0);
	server = accept(listener, NULL, NULL);
	CHECK(server >= 0 && shutdown(client, SHUT_WR) == 0);
	CHECK(read(server, &byte, 1) == 0 && shutdown(server, SHUT_WR) == 0);
	CHECK(read(client, &byte, 1) == 0);
	CHECK(close(client) == 0 && close(server) == 0 && close(listener) == 0);
	probe = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(probe >= 0);
	CHECK(bind(probe, (struct sockaddr *)&local, sizeof(local)) == -1 && errno == EADDRINUSE);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	while (bind(probe, (struct sockaddr *)&local, sizeof(local)) < 0) {
		CHECK(errno == EADDRINUSE);
		CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - start.tv_sec < 125);
		usleep(250000);
	}
	CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - start.tv_sec >= 119);
	CHECK(close(probe) == 0);
}

static void abi_errors(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	int value = 0;
	unsigned long args[] = { fd, SOL_SOCKET, SO_TYPE, (unsigned long)&value };
	socklen_t length = sizeof(value);
	unsigned long option[] = { fd, SOL_SOCKET, SO_TYPE, (unsigned long)&value, (unsigned long)&length };

	CHECK(fd >= 0);
	CHECK(syscall(SYS_socketcall, 15, option) == 0 && value == SOCK_DGRAM);
	option[4] = sizeof(value);
	CHECK(syscall(SYS_socketcall, 14, option) == -1 && errno == ENOPROTOOPT);
	option[2] = SO_REUSEADDR;
	CHECK(syscall(SYS_socketcall, 14, option) == 0);
	CHECK(get_option(fd, SOL_SOCKET, SO_REUSEADDR) == 1);
	value = 0;
	CHECK(syscall(SYS_setsockopt, fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0);
	CHECK(syscall(SYS_getsockopt, fd, SOL_SOCKET, SO_REUSEADDR, &value, &length) == 0 && !value);
	CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) == -1 && errno == ENOPROTOOPT);
	CHECK(getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &((socklen_t){1})) == -1 && errno == EINVAL);
	CHECK(syscall(SYS_socketcall, 15, 1UL) == -1 && errno == EFAULT);
	CHECK(syscall(SYS_socketcall, 21, args) == -1 && errno == ENOSYS);
	CHECK(syscall(SYS_socketcall, 19, args) == -1 && errno == ENOSYS);
	CHECK(syscall(SYS_socketcall, 20, args) == -1 && errno == ENOSYS);
	CHECK(syscall(SYS_recvmmsg, fd, NULL, 0, 0, NULL) == -1 && errno == ENOSYS);
	CHECK(syscall(SYS_sendmmsg, fd, NULL, 0, 0) == -1 && errno == ENOSYS);
	CHECK(syscall(SYS_recvmmsg_time64, fd, NULL, 0, 0, NULL) == -1 && errno == ENOSYS);
	CHECK(close(fd) == 0);
	CHECK(getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &length) == -1 && errno == EBADF);
	CHECK(getsockopt(STDOUT_FILENO, SOL_SOCKET, SO_TYPE, &value, &length) == -1 && errno == ENOTSOCK);
}

static void socketpair_abi(void)
{
	int pair[2];
	unsigned int type = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
	unsigned long args[] = { AF_UNIX, type, 0, 1UL };
	char data;

	CHECK(syscall(SYS_socketpair, AF_INET, type, 0, pair) == -1 && errno == EOPNOTSUPP);
	CHECK(syscall(SYS_socketpair, AF_UNIX, type, 0, 1UL) == -1 && errno == EFAULT);
	CHECK(syscall(SYS_socketcall, 8, args) == -1 && errno == EFAULT);
	for (int legacy = 0; legacy < 2; legacy++) {
		args[3] = (unsigned long)pair;
		CHECK((legacy ? syscall(SYS_socketcall, 8, args) :
		       syscall(SYS_socketpair, AF_UNIX, type, 0, pair)) == 0);
		for (int index = 0; index < 2; index++) {
			CHECK(fcntl(pair[index], F_GETFD) == FD_CLOEXEC);
			CHECK(fcntl(pair[index], F_GETFL) & O_NONBLOCK);
			CHECK(write(pair[index], "!", 1) == 1);
			CHECK(read(pair[1 - index], &data, 1) == 1 && data == '!');
		}
		CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
	}
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "time-wait")) {
		time_wait();
		puts("NET_TIME_WAIT_OK");
		return 0;
	}
	pcb_pressure();
	tcp_semantics();
	tcp_backpressure();
	udp_messages(0);
	udp_messages(1);
	close_and_interrupt();
	io_pressure();
	abi_errors();
	socketpair_abi();
	puts("NET_SEMANTICS_OK");
	return 0;
}
