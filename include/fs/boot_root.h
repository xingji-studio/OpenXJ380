#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <efi/root_binding.h>

// Bootstrap readiness hint only. Strict GPT/UUID/content validation still
// occurs in mount_root(); this read-only observation grants no mount rights.
// Call before the scheduler opens and before partition_init publishes slices.
bool partition_boot_disk_present(const XJ_ROOT_BINDING *binding);
