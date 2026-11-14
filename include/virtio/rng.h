// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

// Boot-time PCI probe. Queues and DMA buffers remain pinned until reboot.
void virtio_rng_init();
// Complete request or wipe the output. Nonblocking calls never wait for DMA.
bool virtio_rng_read(void *output, size_t bytes, bool nonblocking = false);
