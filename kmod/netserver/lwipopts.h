// SPDX-License-Identifier: Apache-2.0
#pragma once

// First-party feature policy. Keep the vendored port's base profile intact.
#include "lwip/include/lwipopts.h"

// Linux listeners set SO_REUSEADDR before binding, including after a restart.
#define SO_REUSE 1

// Route platform diagnostics through first-party policy. Assertions retain
// their original fatal path; routine netconn tracing is opt-in and bounded.
#include "log_policy.h"
#undef LWIP_PLATFORM_DIAG
#define LWIP_PLATFORM_DIAG(arguments) do { netserver_lwip_diag arguments; } while (0)

#undef LWIP_IPV6
#define LWIP_IPV6           1
#define LWIP_IPV6_SCOPES    1
#define LWIP_NETIF_LOOPBACK 1
// Keep ::1 on a separate loopback netif, away from incoming Ethernet packets.
// Public logical interface IDs are independent of private lwIP netif indices.
#undef LWIP_SINGLE_NETIF
#define LWIP_SINGLE_NETIF 0
#undef LWIP_HAVE_LOOPIF
#define LWIP_HAVE_LOOPIF               1
#define LWIP_IPV6_NUM_ADDRESSES        4
#define LWIP_IPV6_AUTOCONFIG           1
#define LWIP_IPV6_SEND_ROUTER_SOLICIT  1
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS  1
#define LWIP_IPV6_MLD                  1
#define LWIP_IPV6_FRAG                 0
#define LWIP_IPV6_REASS                0
#define LWIP_ND6_RDNSS_MAX_DNS_SERVERS 2

// First-party random policy; keep protocol source and vendored defaults intact.
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t kernel_random_u32(void);
#ifdef __cplusplus
}
#endif
#undef LWIP_RAND
#define LWIP_RAND() kernel_random_u32()
#define LWIP_HOOK_TCP_ISN(local_ip, local_port, remote_ip, remote_port) kernel_random_u32()
