#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "resolver:%d: %s (errno %d)\n", __LINE__, #condition, errno); \
		_exit(1); \
	} \
} while (0)

static void transfer(int fd, void *data, size_t len, int send_data)
{
	char *buffer = data;

	while (len) {
		ssize_t count = send_data ? write(fd, buffer, len) : read(fd, buffer, len);
		CHECK(count > 0);
		len -= count;
		buffer += count;
	}
}

static size_t reply(unsigned char *packet, size_t length, int truncated)
{
	static const unsigned char answer[] = {
		0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 42,
	};

	CHECK(length >= 17 && length <= 512 - sizeof(answer));
	CHECK(packet[4] == 0 && packet[5] == 1 && packet[length - 4] == 0 && packet[length - 3] == 1);
	packet[2] = truncated ? 0x83 : 0x81;
	packet[3] = 0x80;
	packet[7] = !truncated;
	if (truncated)
		return length;
	memcpy(packet + length, answer, sizeof(answer));
	return length + sizeof(answer);
}

static void serve(int udp, int tcp)
{
	unsigned char packet[512], prefix[2];
	struct sockaddr_in source;
	socklen_t address_length;
	ssize_t length;
	int stream;

	for (int truncated = 0; truncated < 2; truncated++) {
		address_length = sizeof(source);
		length = recvfrom(udp, packet, sizeof(packet), 0,
				  (struct sockaddr *)&source, &address_length);
		CHECK(length >= 17);
		length = reply(packet, length, truncated);
		CHECK(sendto(udp, packet, length, 0, (struct sockaddr *)&source, address_length) == length);
	}
	stream = accept(tcp, NULL, NULL);
	CHECK(stream >= 0);
	transfer(stream, prefix, sizeof(prefix), 0);
	length = (prefix[0] << 8) | prefix[1];
	CHECK(length >= 17 && length <= sizeof(packet));
	transfer(stream, packet, length, 0);
	length = reply(packet, length, 0);
	prefix[0] = length >> 8;
	prefix[1] = length;
	transfer(stream, prefix, sizeof(prefix), 1);
	transfer(stream, packet, length, 1);
	CHECK(close(stream) == 0 && close(udp) == 0 && close(tcp) == 0);
	_exit(0);
}

static void check_name(const char *name)
{
	struct addrinfo hint = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo *result;
	int status = getaddrinfo(name, "80", &hint, &result);

	if (status)
		fprintf(stderr, "getaddrinfo %s: %s\n", name, gai_strerror(status));
	CHECK(status == 0);
	CHECK(result->ai_family == AF_INET && result->ai_addrlen == sizeof(struct sockaddr_in));
	struct sockaddr_in *address = (struct sockaddr_in *)result->ai_addr;
	CHECK(address->sin_addr.s_addr == htonl(0xc000022a));
	CHECK(address->sin_port == htons(80));
	freeaddrinfo(result);
}

int main(void)
{
	static char config[] = "nameserver 127.0.0.1\noptions timeout:3 attempts:1\n";
	struct sockaddr_in address = {
		.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(53),
	};
	int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int udp = socket(AF_INET, SOCK_DGRAM, 0);
	int tcp = socket(AF_INET, SOCK_STREAM, 0);
	pid_t child;
	int status;

	CHECK(fd >= 0 && udp >= 0 && tcp >= 0);
	transfer(fd, config, sizeof(config) - 1, 1);
	CHECK(close(fd) == 0);
	CHECK(bind(udp, (struct sockaddr *)&address, sizeof(address)) == 0);
	CHECK(bind(tcp, (struct sockaddr *)&address, sizeof(address)) == 0);
	CHECK(listen(tcp, 1) == 0);
	child = fork();
	CHECK(child >= 0);
	if (!child)
		serve(udp, tcp);
	CHECK(close(udp) == 0 && close(tcp) == 0);
	check_name("udp.mario.test.");
	check_name("tcp.mario.test.");
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	puts("NET_RESOLVER_OK");
	return 0;
}
