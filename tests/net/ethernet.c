/*
 * ethernet.py builds this helper into a temporary ramdisk as /mnt/ethernet.
 * Run one mode per invocation inside Mario: config checks the ioctl ABI,
 * stats reports counters, send/udp exercise datagrams, client/server exchange
 * a TCP stream, and poweroff checks shutdown with e1000 attached.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <mario/netctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "ethernet:%d: %s (errno %d)\n", __LINE__, #condition, errno); \
		return 1; \
	} \
} while (0)

/* Match the peer's byte pattern and handle short TCP reads and writes. */
static int transfer(int fd, int send_first, int expect_eof)
{
	unsigned char output[1024], input[1024];

	for (unsigned int round = 0; round < 64; round++) {
		for (unsigned int index = 0; index < sizeof(output); index++)
			output[index] = (round + index) % 251;
		for (unsigned int phase = 0; phase < 2; phase++) {
			int write_data = send_first ? phase == 0 : phase == 1;

			for (size_t offset = 0; offset < sizeof(output);) {
				ssize_t count = write_data ? write(fd, output + offset, sizeof(output) - offset) :
					read(fd, input + offset, sizeof(input) - offset);
				CHECK(count > 0);
				offset += count;
			}
			if (!write_data)
				CHECK(!memcmp(input, output, sizeof(input)));
		}
	}
	CHECK(shutdown(fd, SHUT_WR) == 0);
	/* The forwarded-client case does not require a peer EOF. */
	if (expect_eof)
		CHECK(read(fd, input, sizeof(input)) == 0);
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_in address = { .sin_family = AF_INET };
	char data[64];
	int fd;

	CHECK(argc >= 2);
	if (!strcmp(argv[1], "poweroff")) {
		CHECK(reboot(RB_POWER_OFF) == 0);
		return 1;
	}
	if (!strcmp(argv[1], "stats")) {
		struct mario_net_stats stats = { .version = MARIO_NET_STATS_VERSION };
		char ip[INET_ADDRSTRLEN];

		CHECK((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
		CHECK(ioctl(fd, MARIO_NET_GET_STATS, &stats) == 0);
		CHECK(inet_ntop(AF_INET, &stats.address, ip, sizeof(ip)));
		printf("NET_STATS flags=%u address=%s/%u rx=%" PRIu64 " tx=%" PRIu64 "\n",
			stats.flags, ip, stats.prefix, stats.rx_bytes, stats.tx_bytes);
		return 0;
	}
	if (!strcmp(argv[1], "config")) {
		struct mario_net_status status = { .config.version = MARIO_NET_VERSION };
		struct mario_net_config invalid;
		struct mario_net_stats invalid_stats = { .version = MARIO_NET_STATS_VERSION + 1 };

		CHECK((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
		CHECK(ioctl(fd, MARIO_NET_GET, &status) == 0);
		invalid = status.config;
		invalid.flags = MARIO_NET_UP;
		invalid.version++;
		CHECK(ioctl(fd, MARIO_NET_SET, &invalid) == -1 && errno == EINVAL);
		invalid.version = MARIO_NET_VERSION;
		invalid.prefix = 33;
		CHECK(ioctl(fd, MARIO_NET_SET, &invalid) == -1 && errno == EINVAL);
		CHECK(ioctl(fd, MARIO_NET_SET, 0x80000000UL) == -1 && errno == EFAULT);
		CHECK(ioctl(fd, MARIO_NET_GET, 0x80000000UL) == -1 && errno == EFAULT);
		CHECK(ioctl(fd, MARIO_NET_GET_STATS, 0x80000000UL) == -1 && errno == EFAULT);
		CHECK(ioctl(fd, MARIO_NET_GET_STATS, &invalid_stats) == -1 && errno == EINVAL);
		return 0;
	}
	CHECK(argc >= 3);
	address.sin_port = htons(strtoul(argv[2], NULL, 10));
	if (argc > 3)
		CHECK(inet_pton(AF_INET, argv[3], &address.sin_addr) == 1);
	if (!strcmp(argv[1], "send")) {
		CHECK((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
		CHECK(sendto(fd, "mario-wire", 10, 0, (void *)&address, sizeof(address)) == 10);
	} else if (!strcmp(argv[1], "udp")) {
		socklen_t size = sizeof(address);
		ssize_t length;

		CHECK((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
		CHECK(bind(fd, (void *)&address, sizeof(address)) == 0);
		/* Let the host peer send only after the socket is bound. */
		puts("ETH_READY");
		fflush(stdout);
		CHECK((length = recvfrom(fd, data, sizeof(data), 0, (void *)&address, &size)) == 10);
		CHECK(!memcmp(data, "local-peer", 10));
		CHECK(sendto(fd, data, length, 0, (void *)&address, size) == length);
	} else if (!strcmp(argv[1], "client")) {
		CHECK((fd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
		CHECK(connect(fd, (void *)&address, sizeof(address)) == 0);
		return transfer(fd, 1, argc == 4);
	} else if (!strcmp(argv[1], "server")) {
		CHECK((fd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
		CHECK(bind(fd, (void *)&address, sizeof(address)) == 0);
		CHECK(listen(fd, 1) == 0);
		puts("ETH_READY");
		fflush(stdout);
		CHECK((fd = accept(fd, NULL, NULL)) >= 0);
		return transfer(fd, 0, 1);
	} else {
		return 2;
	}
	return 0;
}
