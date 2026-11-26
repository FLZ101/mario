#include <arpa/inet.h>
#include <errno.h>
#include <mario/netctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "interface.h"
#include "net-support.h"
#include "version.h"

static const uint32_t loop_network = 0x7f000000;
static const uint32_t loop_mask = 0xff000000;

enum view {
    INTERFACE,
    INTERFACE_TABLE,
    ROUTE_TABLE,
};

static void set_ipv4(struct sockaddr_storage *storage, uint32_t address)
{
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)storage;

    ipv4->sin_family = AF_INET;
    ipv4->sin_addr.s_addr = address;
}

static uint32_t prefix_mask(uint32_t prefix)
{
    return prefix && prefix <= 32 ? htonl(UINT32_MAX << (32 - prefix)) : 0;
}

static int read_ethernet(struct mario_net_status *status, struct mario_net_stats *stats)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int result;

    if (fd < 0)
        return -1;
    result = ioctl(fd, MARIO_NET_GET, status);
    if (result == 0 && stats && (status->config.flags & MARIO_NET_PRESENT))
        result = ioctl(fd, MARIO_NET_GET_STATS, stats);
    int error = errno;
    close(fd);
    errno = error;
    return result;
}

static void show_loopback(void)
{
    struct interface loopback = {
        .name = "lo",
        .type = ARPHRD_LOOPBACK,
        .flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING,
        .tx_queue_len = -1,
        .has_ip = 1,
    };

    set_ipv4(&loopback.addr_sas, htonl(loop_network | 1));
    set_ipv4(&loopback.netmask_sas, htonl(loop_mask));
    ife_print(&loopback);
}

static void show_ethernet(const struct mario_net_status *status,
                          const struct mario_net_stats *stats)
{
    const struct mario_net_config *config = &status->config;
    uint32_t mask = prefix_mask(config->prefix);
    struct interface ethernet = {
        .name = "eth0",
        .type = ARPHRD_ETHER,
        .flags = IFF_BROADCAST,
        .mtu = 1500,
        .tx_queue_len = -1,
        .has_ip = config->address != 0,
        .statistics_valid = 1,
        .stats = {
            .rx_packets = status->rx_packets,
            .tx_packets = status->tx_packets,
            .rx_bytes = stats->rx_bytes,
            .tx_bytes = stats->tx_bytes,
            .rx_dropped = status->rx_dropped,
            .rx_fifo_errors = status->rx_overruns,
            .tx_errors = status->tx_errors,
        },
    };

    if (config->flags & MARIO_NET_UP)
        ethernet.flags |= IFF_UP;
    if (config->flags & MARIO_NET_CARRIER)
        ethernet.flags |= IFF_RUNNING;
    memcpy(ethernet.hwaddr, status->mac, sizeof(status->mac));
    set_ipv4(&ethernet.addr_sas, config->address);
    set_ipv4(&ethernet.netmask_sas, mask);
    set_ipv4(&ethernet.broadaddr_sas, config->address | ~mask);
    ife_print(&ethernet);
}

static void show_routes(const struct mario_net_config *config)
{
    const uint32_t active = MARIO_NET_PRESENT | MARIO_NET_UP | MARIO_NET_CARRIER;
    uint32_t mask = prefix_mask(config->prefix);

    mario_fib_header();
    if (config->address && (config->flags & active) == active) {
        if (config->gateway)
            mario_fib_entry(0, config->gateway, 0, "eth0");
        mario_fib_entry(config->address & mask, 0, mask, "eth0");
    }
    mario_fib_entry(htonl(loop_network), 0, htonl(loop_mask), "lo");
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    const char *usage;
    enum view view = INTERFACE;
    struct mario_net_status status = { .config.version = MARIO_NET_VERSION };
    struct mario_net_stats stats = { .version = MARIO_NET_STATS_VERSION };
    bool all = false, ethernet = true, loopback = true, valid;

    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "ifconfig")) {
        usage = "ifconfig [-a | eth0 | lo]";
        all = argc == 2 && !strcmp(argv[1], "-a");
        valid = argc == 1 || all;
        if (argc == 2 && !strcmp(argv[1], "eth0")) {
            loopback = false;
            valid = true;
        } else if (argc == 2 && !strcmp(argv[1], "lo")) {
            ethernet = false;
            valid = true;
        }
    } else if (!strcmp(name, "route")) {
        usage = "route -n";
        view = ROUTE_TABLE;
        valid = argc == 2 && !strcmp(argv[1], "-n");
    } else if (!strcmp(name, "netstat")) {
        usage = "netstat [-i | -rn]";
        all = true;
        view = argc == 2 && !strcmp(argv[1], "-i") ? INTERFACE_TABLE : ROUTE_TABLE;
        valid = argc == 2 && (view == INTERFACE_TABLE || !strcmp(argv[1], "-rn") ||
                             !strcmp(argv[1], "-nr"));
    } else {
        fputs("Invoke as ifconfig, route or netstat.\n", stderr);
        return 2;
    }

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("Usage: %s (read-only)\n", usage);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts(RELEASE);
        return 0;
    }
    if (!valid) {
        fprintf(stderr, "%s: unsupported arguments; use %s\n", name, usage);
        return 2;
    }
    if (ethernet && read_ethernet(&status, view == INTERFACE ? &stats : NULL) < 0) {
        perror(name);
        return 1;
    }
    if (!loopback && !(status.config.flags & MARIO_NET_PRESENT)) {
        fprintf(stderr, "%s: eth0: device not found\n", name);
        return 1;
    }
    if (view == ROUTE_TABLE) {
        show_routes(&status.config);
    } else {
        if (view == INTERFACE_TABLE) {
            ife_short = 1;
            puts("Kernel Interface table");
            puts("Iface             MTU    RX-OK RX-ERR RX-DRP RX-OVR    TX-OK TX-ERR TX-DRP TX-OVR Flg");
        }
        if ((status.config.flags & MARIO_NET_PRESENT) &&
            ((status.config.flags & MARIO_NET_UP) || all || !loopback))
            show_ethernet(&status, &stats);
        if (loopback)
            show_loopback();
    }
    return 0;
}
