#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef ssize_t (*receive_fn)(int, void *, size_t, int, struct sockaddr *, socklen_t *);

static ssize_t direct_receive(int descriptor, void *buffer, size_t length, int flags,
			      struct sockaddr *address, socklen_t *address_length)
{
	return syscall(SYS_recvfrom, descriptor, buffer, length, flags,
		       address, address_length);
}

static ssize_t legacy_receive(int descriptor, void *buffer, size_t length, int flags,
			      struct sockaddr *address, socklen_t *address_length)
{
	unsigned long arguments[] = { descriptor, (unsigned long)buffer, length,
		flags, (unsigned long)address, (unsigned long)address_length };

	return syscall(SYS_socketcall, 12, arguments);
}

static void test_stream(receive_fn receive)
{
	static char large[4096];
	char buffer[32];
	struct sockaddr_un address;
	socklen_t address_length;
	int descriptors[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, descriptors) == 0);
	assert(receive(descriptors[1], NULL, 0, 0, NULL, (socklen_t *)-1) == 0);
	assert(receive(descriptors[1], buffer, sizeof(buffer), 0, NULL, NULL) == -1);
	assert(errno == EAGAIN);
	assert(write(descriptors[0], "abcdef", 6) == 6);
	assert(receive(descriptors[1], buffer, 3, MSG_PEEK, NULL, NULL) == 3);
	assert(!memcmp(buffer, "abc", 3));
	assert(receive(descriptors[1], buffer, 3, MSG_PEEK, NULL, NULL) == 3);
	assert(!memcmp(buffer, "abc", 3));
	assert(receive(descriptors[1], buffer, 2, 0, NULL, NULL) == 2);
	assert(!memcmp(buffer, "ab", 2));
	assert(receive(descriptors[1], buffer, sizeof(buffer), MSG_PEEK, NULL, NULL) == 4);
	assert(!memcmp(buffer, "cdef", 4));
	assert(receive(descriptors[1], buffer, sizeof(buffer), 0, NULL, NULL) == 4);
	assert(!memcmp(buffer, "cdef", 4));
	memset(large, 'a', sizeof(large));
	assert(write(descriptors[0], large, sizeof(large) - 6) == sizeof(large) - 6);
	assert(receive(descriptors[1], large, sizeof(large) - 8, 0, NULL, NULL) ==
	       sizeof(large) - 8);
	assert(write(descriptors[0], "bbbbbbbbbbbbbbbbbbbb", 20) == 20);
	assert(receive(descriptors[1], buffer, sizeof(buffer), MSG_PEEK, NULL, NULL) == 22);
	assert(!memcmp(buffer, "aabbbbbbbbbbbbbbbbbbbb", 22));
	assert(receive(descriptors[1], buffer, sizeof(buffer), 0, NULL, NULL) == 22);
	assert(!memcmp(buffer, "aabbbbbbbbbbbbbbbbbbbb", 22));
	assert(write(descriptors[0], "x", 1) == 1);
	memset(&address, 0xa5, sizeof(address));
	address_length = sizeof(address);
	assert(receive(descriptors[1], buffer, 1, 0,
		       (struct sockaddr *)&address, &address_length) == 1);
	assert(address_length == 0 && ((unsigned char *)&address)[0] == 0xa5);
	assert(shutdown(descriptors[0], SHUT_WR) == 0);
	assert(receive(descriptors[1], buffer, 1, 0, NULL, NULL) == 0);
	assert(close(descriptors[0]) == 0);
	assert(receive(descriptors[1], buffer, 1, MSG_PEEK, NULL, NULL) == 0);
	assert(close(descriptors[1]) == 0);
}

static void test_errors(receive_fn receive)
{
	char buffer[8];
	struct sockaddr_un address;
	int descriptors[2];
	int descriptor;
	int original_flags;

	assert(receive(-1, buffer, 1, 0, NULL, NULL) == -1 && errno == EBADF);
	descriptor = open("/dev/null", O_RDONLY);
	assert(descriptor >= 0);
	assert(receive(descriptor, buffer, 1, 0, NULL, NULL) == -1 && errno == ENOTSOCK);
	assert(close(descriptor) == 0);
	assert(receive(descriptor, buffer, 1, 0, NULL, NULL) == -1 && errno == EBADF);
	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(descriptor >= 0);
	assert(receive(descriptor, buffer, 1, 0, NULL, NULL) == -1 && errno == ENOTCONN);
	assert(close(descriptor) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
	original_flags = fcntl(descriptors[1], F_GETFL);
	assert(original_flags >= 0 && !(original_flags & O_NONBLOCK));
	assert(receive(descriptors[1], buffer, 1, MSG_DONTWAIT, NULL, NULL) == -1);
	assert(errno == EAGAIN && fcntl(descriptors[1], F_GETFL) == original_flags);
	assert(receive(descriptors[1], buffer, 1, MSG_PEEK | MSG_DONTWAIT, NULL, NULL) == -1);
	assert(errno == EAGAIN);
	assert(write(descriptors[0], "q", 1) == 1);
	assert(receive(descriptors[1], buffer, 1, MSG_OOB, NULL, NULL) == -1);
	assert(errno == EOPNOTSUPP);
	assert(receive(descriptors[1], buffer, 1, MSG_WAITALL, NULL, NULL) == -1);
	assert(errno == EOPNOTSUPP);
	assert(receive(descriptors[1], buffer, 1, INT_MIN, NULL, NULL) == -1);
	assert(errno == EOPNOTSUPP);
	assert(receive(descriptors[1], NULL, 1, 0, NULL, NULL) == -1 && errno == EFAULT);
	assert(receive(descriptors[1], (void *)-1, 1, 0, NULL, NULL) == -1 && errno == EFAULT);
	assert(receive(descriptors[1], buffer, (size_t)-1, 0, NULL, NULL) == -1 && errno == EINVAL);
	assert(receive(descriptors[1], buffer, 1, 0, (struct sockaddr *)&address, NULL) == -1);
	assert(errno == EFAULT);
	assert(receive(descriptors[1], buffer, 1, 0,
		       (struct sockaddr *)&address, (socklen_t *)-1) == -1 && errno == EFAULT);
	assert(receive(descriptors[1], buffer, 1, MSG_PEEK | MSG_DONTWAIT, NULL, NULL) == 1);
	assert(buffer[0] == 'q');
	assert(receive(descriptors[1], buffer, sizeof(buffer), 0, NULL, (socklen_t *)-1) == 1);
	assert(buffer[0] == 'q');
	assert(shutdown(descriptors[1], SHUT_RD) == 0);
	assert(receive(descriptors[1], buffer, 1, 0, NULL, NULL) == 0);
	assert(close(descriptors[0]) == 0);
	assert(close(descriptors[1]) == 0);
}

static void test_addresses(receive_fn receive)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct sockaddr_un source;
	socklen_t expected_length;
	socklen_t source_length;
	char buffer;
	int listener = socket(AF_UNIX, SOCK_STREAM, 0);
	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	int server;
	long page_size = sysconf(_SC_PAGESIZE);
	void *read_only;

	assert(listener >= 0 && client >= 0 && page_size > 0);
	assert(snprintf(address.sun_path, sizeof(address.sun_path),
			"/tmp/recvfrom-%d", getpid()) > 0);
	expected_length = offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1;
	assert(bind(listener, (struct sockaddr *)&address, expected_length) == 0);
	assert(listen(listener, 1) == 0);
	assert(connect(client, (struct sockaddr *)&address, expected_length) == 0);
	server = accept(listener, NULL, NULL);
	assert(server >= 0);
	assert(close(listener) == 0);
	assert(write(server, "abcdef", 6) == 6);
	memset(&source, 0xa5, sizeof(source));
	source_length = sizeof(source);
	assert(receive(client, &buffer, 1, MSG_PEEK,
		       (struct sockaddr *)&source, &source_length) == 1 && buffer == 'a');
	assert(source_length == expected_length && source.sun_family == AF_UNIX);
	assert(!strcmp(source.sun_path, address.sun_path));
	assert(((unsigned char *)&source)[expected_length] == 0xa5);
	memset(&source, 0xa5, sizeof(source));
	source_length = 3;
	assert(receive(client, &buffer, 1, 0,
		       (struct sockaddr *)&source, &source_length) == 1 && buffer == 'a');
	assert(source_length == expected_length && source.sun_family == AF_UNIX);
	assert(source.sun_path[0] == '/' && (unsigned char)source.sun_path[1] == 0xa5);
	source_length = 0;
	assert(receive(client, &buffer, 1, 0,
		       (struct sockaddr *)-1, &source_length) == 1 && buffer == 'b');
	assert(source_length == expected_length);
	source_length = 1;
	assert(receive(client, &buffer, 1, 0, (struct sockaddr *)-1, &source_length) == -1);
	assert(errno == EFAULT && source_length == 1);
	source_length = (socklen_t)-1;
	assert(receive(client, &buffer, 1, 0, (struct sockaddr *)&source, &source_length) == -1);
	assert(errno == EINVAL);
	read_only = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(read_only != MAP_FAILED);
	*(socklen_t *)read_only = sizeof(source);
	assert(mprotect(read_only, page_size, PROT_READ) == 0);
	assert(receive(client, &buffer, 1, 0,
		       (struct sockaddr *)&source, read_only) == -1 && errno == EFAULT);
	source_length = sizeof(source);
	assert(receive(client, &buffer, 1, 0, read_only, &source_length) == -1 && errno == EFAULT);
	assert(receive(client, read_only, 1, 0, NULL, NULL) == -1 && errno == EFAULT);
	assert(munmap(read_only, page_size) == 0);
	source_length = sizeof(source);
	assert(receive(client, &buffer, 1, 0,
		       (struct sockaddr *)&source, &source_length) == 1 && buffer == 'c');
	assert(source_length == expected_length);
	assert(receive(client, &buffer, 1, 0, NULL, &source_length) == 1 && buffer == 'd');
	assert(source_length == expected_length);
	assert(close(server) == 0);
	assert(receive(client, &buffer, 1, 0, NULL, NULL) == 1 && buffer == 'e');
	assert(receive(client, &buffer, 1, 0, NULL, NULL) == 1 && buffer == 'f');
	assert(receive(client, &buffer, 1, 0, NULL, NULL) == 0);
	assert(close(client) == 0);
}

static void test_rights(receive_fn receive)
{
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = { 0 };
	char buffer;
	struct iovec vector = { .iov_base = &buffer, .iov_len = 1 };
	struct msghdr message = { .msg_iov = &vector, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	struct stat status;
	int descriptor = open("/dev/null", O_RDONLY);
	int descriptors[2];
	int received;

	assert(descriptor >= 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
	header->cmsg_len = CMSG_LEN(sizeof(descriptor));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
	buffer = 'r';
	assert(sendmsg(descriptors[0], &message, 0) == 1);
	assert(receive(descriptors[1], &buffer, 1, MSG_PEEK, NULL, NULL) == 1 && buffer == 'r');
	assert(recvmsg(descriptors[1], &message, MSG_CMSG_CLOEXEC) == 1 && buffer == 'r');
	header = CMSG_FIRSTHDR(&message);
	assert(header != NULL && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
	memcpy(&received, CMSG_DATA(header), sizeof(received));
	assert(received >= 0 && received != descriptor);
	assert(fcntl(received, F_GETFD) & FD_CLOEXEC);
	assert(fstat(received, &status) == 0 && S_ISCHR(status.st_mode));
	assert(close(received) == 0);
	assert(close(descriptor) == 0);
	assert(close(descriptors[0]) == 0);
	assert(close(descriptors[1]) == 0);
}

static void test_blocking(receive_fn receive)
{
	int descriptors[2];
	int status;
	char buffer[8];
	pid_t child;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
	child = fork();
	assert(child >= 0);
	if (!child) {
		assert(close(descriptors[1]) == 0);
		assert(write(descriptors[0], "wake", 4) == 4);
		assert(close(descriptors[0]) == 0);
		_exit(0);
	}
	assert(close(descriptors[0]) == 0);
	assert(receive(descriptors[1], buffer, sizeof(buffer), 0, NULL, NULL) == 4);
	assert(!memcmp(buffer, "wake", 4));
	assert(receive(descriptors[1], buffer, 1, 0, NULL, NULL) == 0);
	assert(close(descriptors[1]) == 0);
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_argument_boundaries(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	char *mapping;
	unsigned long *arguments;
	int descriptors[2];
	char buffer;

	assert(page_size > 0);
	mapping = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(mapping != MAP_FAILED);
	assert(munmap(mapping + page_size, page_size) == 0);
	arguments = (unsigned long *)(mapping + page_size - 4 * sizeof(*arguments));
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
	arguments[0] = descriptors[1];
	arguments[1] = (unsigned long)&buffer;
	arguments[2] = 1;
	arguments[3] = MSG_DONTWAIT;
	assert(write(descriptors[0], "g", 1) == 1);
	assert(syscall(SYS_socketcall, 12, arguments) == -1 && errno == EFAULT);
	assert(syscall(SYS_socketcall, 10, arguments) == 1 && buffer == 'g');
	assert(syscall(SYS_socketcall, 10, arguments) == -1 && errno == EAGAIN);
	arguments[3] = MSG_OOB;
	assert(syscall(SYS_socketcall, 10, arguments) == -1 && errno == EOPNOTSUPP);
	arguments += 2;
	arguments[0] = descriptors[1];
	arguments[1] = SHUT_RD;
	assert(syscall(SYS_socketcall, 13, arguments) == 0);
	assert(syscall(SYS_socketcall, 12, NULL) == -1 && errno == EFAULT);
	assert(syscall(SYS_socketcall, 0, NULL) == -1 && errno == ENOSYS);
	assert(close(descriptors[0]) == 0);
	assert(close(descriptors[1]) == 0);
	assert(munmap(mapping, page_size * 2) == 0);
}

int main(void)
{
	static const receive_fn receivers[] = { direct_receive, legacy_receive, recvfrom };

	for (size_t index = 0; index < sizeof(receivers) / sizeof(receivers[0]); index++) {
		test_stream(receivers[index]);
		test_errors(receivers[index]);
		test_addresses(receivers[index]);
		test_rights(receivers[index]);
		test_blocking(receivers[index]);
	}
	test_argument_boundaries();
	puts("socket-recvfrom: direct, legacy, and libc receive paths passed");
	return 0;
}
