#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include <efi/root_binding.h>

namespace xjinstaller {

using EspVerifySize = __SIZE_TYPE__;

struct EspReadDevice
{
    bool (*read)(void *context, uint64_t offset, void *data, EspVerifySize bytes);
    void *context;
    uint64_t bytes;
};

struct EspExpectedFiles
{
    const uint8_t *loader;
    EspVerifySize loader_bytes;
    const uint8_t *kernel;
    EspVerifySize kernel_bytes;
};

struct EspVerifyWorkspace
{
    // Caller-owned heap buffers, disjoint from inputs and publication. This
    // conservative bitset budget is computable before the first device read.
    uint8_t *cluster_bits;
    EspVerifySize cluster_bits_bytes;
    uint8_t boot[512], data[512], fat[512], mirror[512], binding_sector[512];
    uint64_t read_requests;
    bool started, io_failed, complete;
};

struct EspPublication
{
    uint64_t sector_offset; // Relative to the authenticated ESP window.
    uint8_t sector[512];   // Only the first 96 bytes differ from the placeholder.
    bool ready;
};

// Zero means unsupported geometry. No allocation or callbacks.
EspVerifySize esp_verify_bitmap_bytes(uint64_t device_bytes);

// Read-only verification of a fresh minimal mirrored FAT32 ESP, NOT a general
// FAT driver or boot activation. The exact five directories and three files
// must have disjoint complete chains, no other allocations, exact payloads,
// clean FAT and valid BPB/FSInfo/backup metadata. root.bin must be 96 zero bytes.
// The result contains one proposed sector; this function never writes/flushes.
// Workspace is one-shot after its first read, including a failed read. The
// callback must stop on failure; retries require the caller's recovery policy.
// Caller authenticates/pins its exclusive device window and immutable buffers,
// and completes GPT/ext4/readback/durability gates separately. Only after every
// writer has closed/unmounted may it submit the proposed sector and real flush.
// Arithmetic checks cannot establish that arbitrary pointers are mapped.
int esp_verify_prepare_publication(const EspReadDevice &, const EspExpectedFiles &, const XJ_ROOT_BINDING &,
                                   EspVerifyWorkspace &, EspPublication &);

} // namespace xjinstaller
