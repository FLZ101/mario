#ifndef MARIO_LWIP_HOOKS_H
#define MARIO_LWIP_HOOKS_H

#include <lwip/sys.h>

struct netif;
struct pbuf;

void ethernet_dhcp_option(struct netif *iface, unsigned int option, unsigned int length,
			  const struct pbuf *packet, unsigned int offset);

#define LWIP_HOOK_DHCP_PARSE_OPTION(iface, client, state, msg, type, option, length, packet, offset) \
	((void)(msg), ethernet_dhcp_option(ip_current_input_netif(), option, length, packet, offset))

#define DHCP_GLOBAL_XID (sys_now())
#define DHCP_DEFINE_CUSTOM_TIMEOUTS
#define DHCP_SET_TIMEOUT_FROM_OFFERED_T0_LEASE(result, client) \
	((result) = LWIP_MAX(1U, (client)->offered_t0_lease))
#define DHCP_SET_TIMEOUT_FROM_OFFERED_T1_RENEW(result, client) \
	((result) = LWIP_MAX(1U, (client)->offered_t1_renew))
#define DHCP_SET_TIMEOUT_FROM_OFFERED_T2_REBIND(result, client) \
	((result) = LWIP_MAX(1U, (client)->offered_t2_rebind))
#define DHCP_NEXT_TIMEOUT_THRESHOLD 60
#define DHCP_REQUEST_BACKOFF_SEQUENCE(tries) ((u16_t)(((tries) < 6 ? 1U << (tries) : 60U) * 1000U))

#endif
