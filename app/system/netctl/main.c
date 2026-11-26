#include <arpa/inet.h>
#include <errno.h>
#include <mario/netctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_config(char *cidr, const char *gateway, struct mario_net_config *config)
{
	char *slash = strchr(cidr, '/');
	char *end;
	unsigned long prefix;

	if (!slash || slash[1] < '0' || slash[1] > '9')
		return -1;
	*slash = '\0';
	prefix = strtoul(slash + 1, &end, 10);
	if (*end || !prefix || prefix > 32 ||
	    inet_pton(AF_INET, cidr, &config->address) != 1 || !config->address)
		return -1;
	config->prefix = prefix;
	config->gateway = 0;
	return gateway && inet_pton(AF_INET, gateway, &config->gateway) != 1 ? -1 : 0;
}

static void print_status(const struct mario_net_status *status)
{
	char address[INET_ADDRSTRLEN], gateway[INET_ADDRSTRLEN];
	uint32_t flags = status->config.flags;
	const uint8_t *mac = status->mac;

	if (!(flags & MARIO_NET_PRESENT)) {
		if (flags & MARIO_NET_PROBE_FAILED)
			printf("eth0: probe failed: %s\n", strerror(status->probe_error));
		else
			puts("eth0: not present");
		return;
	}
	inet_ntop(AF_INET, &status->config.address, address, sizeof(address));
	inet_ntop(AF_INET, &status->config.gateway, gateway, sizeof(gateway));
	printf("eth0: %s carrier=%s source=%s address=%s/%u gateway=%s\n",
	       (flags & MARIO_NET_UP) ? "up" : "down", (flags & MARIO_NET_CARRIER) ? "up" : "down",
	       status->config.address ? "static" : "none", address, status->config.prefix, gateway);
	printf("mac=%02x:%02x:%02x:%02x:%02x:%02x irq=%u interrupts=%u\n",
	       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], status->irq, status->interrupts);
	printf("rx_packets=%u rx_dropped=%u rx_overruns=%u rx_high_water=%u rx_ring_size=%u\n",
	       status->rx_packets, status->rx_dropped, status->rx_overruns, status->rx_high_water,
	       status->rx_ring_size);
	printf("tx_packets=%u tx_errors=%u tx_busy=%u tx_high_water=%u tx_ring_size=%u\n",
	       status->tx_packets, status->tx_errors, status->tx_busy, status->tx_high_water,
	       status->tx_ring_size);
}

int main(int argc, char **argv)
{
	struct mario_net_status status = { .config.version = MARIO_NET_VERSION };
	struct mario_net_config config;
	int down = argc == 2 && !strcmp(argv[1], "down");
	int show = argc == 1 || (argc == 2 && !strcmp(argv[1], "status"));
	int up = argc >= 2 && argc <= 4 && !strcmp(argv[1], "up");
	int fd;

	if (!down && !show && !up)
		goto usage;
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || ioctl(fd, MARIO_NET_GET, &status) < 0) {
		perror("netctl: status");
		return 1;
	}
	if (show) {
		print_status(&status);
		return 0;
	}
	config = status.config;
	config.flags = up ? MARIO_NET_UP : 0;
	if (argc >= 3 && parse_config(argv[2], argc == 4 ? argv[3] : NULL, &config))
		goto usage;
	if (ioctl(fd, MARIO_NET_SET, &config) < 0) {
		perror("netctl: configure");
		return 1;
	}
	return 0;

usage:
	fprintf(stderr, "usage: netctl [status | down | up [IPv4/prefix [gateway]]]\n");
	return 2;
}
