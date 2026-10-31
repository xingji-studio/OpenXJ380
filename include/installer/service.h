#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// All addresses are user addresses in the current process. The handler copies
// a fixed request once; worker arguments contain only a static kernel ticket.
uint64_t do_xapi_InstallerNative(uint64_t operation, uint64_t request, uint64_t request_bytes,
                                uint64_t response, uint64_t response_bytes);
