#ifndef MARIO_LWIPOPTS_H
#define MARIO_LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_RAW 0
#define LWIP_IGMP 0
#define LWIP_DHCP 0
#define LWIP_DNS 0
#define IP_FRAG 0
#define IP_REASSEMBLY 0

#define MEM_ALIGNMENT 4
#define MEM_SIZE (32 * 1024)
#define MEMP_NUM_TCP_PCB 24
#define MEMP_NUM_TCP_PCB_LISTEN 8
#define MEMP_NUM_TCP_SEG 64
#define MEMP_NUM_UDP_PCB 16
#define PBUF_POOL_SIZE 32

#define mem_init lwip_mem_init

#endif
