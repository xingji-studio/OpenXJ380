// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lwip/include/lwipopts.h"

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
