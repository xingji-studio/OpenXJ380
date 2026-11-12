// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#include <epf_format.h>

struct process_control_block;
// The caller owns a private exec image or pins the target PCB/address space
// with its preparation ticket.
// Success returns a user entry VA; errors use the existing negative-errno ABI.
// Returns 1 for the ABI's one accepted INTERP path, 0 when absent, or -errno.
int epf_get_interpreter(const uint8_t *file, uint64_t size);
uint64_t load_user_epf_image(const uint8_t *file, uint64_t size, process_control_block *process,
                             const uint8_t *interpreter = nullptr, uint64_t interpreter_size = 0);
