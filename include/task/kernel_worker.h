#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// Dedicated boot-lived kernel_group worker. Allocates all resources before
// queue/scheduler publication, never under a spin lock or nested preemption
// gate. Returns a TID (zero is valid) or size_t(-errno). Arguments must remain
// kernel-owned until the worker consumes them. No user-pointer/credential ABI.
__SIZE_TYPE__ create_kernel_worker(void *entry, void *argument, const char *name);
