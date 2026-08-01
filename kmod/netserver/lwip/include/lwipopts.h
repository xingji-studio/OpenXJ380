#pragma once

#include "arch/cc.h"

#define NO_SYS 0
#define SYS_LIGHTWEIGHT_PROT 1
#define LWIP_PROVIDE_ERRNO 1
#define LWIP_NOASSERT 1
#define LWIP_DEBUG 0

#define LWIP_NETCONN 1
#define LWIP_NETIF_API 1
#define LWIP_SOCKET 0
#define LWIP_COMPAT_SOCKETS 0

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_RAW 0

#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 8
#define LWIP_STATS 0

#define LWIP_SINGLE_NETIF 1
#define LWIP_HAVE_LOOPIF 0
#define LWIP_NETIF_HOSTNAME 0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0

#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define ETH_PAD_SIZE 0
#define LWIP_CHKSUM_ALGORITHM 3

#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 1700

#define TCP_MSS 1460
#define TCP_WND (16 * TCP_MSS)
#define TCP_SND_BUF 8192
#define TCP_SND_QUEUELEN 16
#define TCP_LISTEN_BACKLOG 1

#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_SYS_TIMEOUT 32
#define MEMP_NUM_TCPIP_MSG_API 32
#define MEMP_NUM_TCPIP_MSG_INPKT 32

#define TCPIP_MBOX_SIZE 32
#define DEFAULT_ACCEPTMBOX_SIZE 16
#define DEFAULT_TCP_RECVMBOX_SIZE 16

#define LWIP_PLATFORM_ASSERT(message)                 \
    do {                                              \
        printk("lwIP assert: %s\n", message);         \
        for (;;) {                                    \
            delay_ms_hp(1000);                        \
        }                                             \
    } while (0)

#define LWIP_PLATFORM_DIAG(arguments) \
    do {                              \
        printk arguments;             \
    } while (0)

#define LWIP_RAND() ((u32_t)nanoTime())
