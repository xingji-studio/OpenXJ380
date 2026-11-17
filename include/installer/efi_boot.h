#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>

namespace xjinstaller {
// Pure EFI_LOAD_OPTION encoding and BootOrder insertion, used by the bounded
// runtime adapter and host tests. No firmware or disk I/O occurs here.
int efi_boot_option(unsigned partition, uint64_t first, uint64_t sectors, const uint8_t guid[16], uint8_t *output,
                    __SIZE_TYPE__ capacity, __SIZE_TYPE__ &bytes);
int efi_boot_order(const uint8_t *before, __SIZE_TYPE__ before_bytes, uint16_t slot, uint8_t *output,
                   __SIZE_TYPE__ capacity, __SIZE_TYPE__ &bytes);
} // namespace xjinstaller
