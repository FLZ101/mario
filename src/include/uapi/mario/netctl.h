#ifndef MARIO_UAPI_NETCTL_H
#define MARIO_UAPI_NETCTL_H

#ifndef __KERNEL__
#include <stdint.h>
#endif

#define MARIO_NET_VERSION 1U
#define MARIO_NET_GET 0xc04c4e01U
#define MARIO_NET_SET 0x40144e02U

#define MARIO_NET_PRESENT 1U
#define MARIO_NET_UP 2U
#define MARIO_NET_CARRIER 4U
#define MARIO_NET_PROBE_FAILED 8U

struct mario_net_config {
	uint32_t version;
	uint32_t flags;
	uint32_t address;
	uint32_t prefix;
	uint32_t gateway;
};

struct mario_net_status {
	struct mario_net_config config;
	uint8_t mac[6];
	uint8_t irq;
	uint8_t reserved;
	uint32_t rx_packets;
	uint32_t tx_packets;
	uint32_t rx_dropped;
	uint32_t tx_busy;
	uint32_t rx_high_water;
	uint32_t tx_high_water;
	uint32_t rx_overruns;
	uint32_t interrupts;
	uint32_t rx_ring_size;
	uint32_t tx_ring_size;
	uint32_t probe_error;
	uint32_t tx_errors;
};

#endif
