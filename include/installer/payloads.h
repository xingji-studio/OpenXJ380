#pragma once
// SPDX-License-Identifier: Apache-2.0
// Two immutable target profiles share the existing single EFI-owned system span.
// All offsets are relative to that span; no pointer or native struct is serialized.

typedef __UINT64_TYPE__ xj_payload_u64;
typedef __SIZE_TYPE__ xj_payload_size;

#define XJ_INSTALLER_PAYLOAD_HEADER_BYTES 64U
#define XJ_INSTALLER_PAYLOAD_MAGIC "XJDUAL01"

typedef struct xj_installer_payloads
{
    const unsigned char *ext4, *fat32;
    xj_payload_size ext4_bytes, fat32_bytes;
} xj_installer_payloads;

static inline xj_payload_u64 xj_payload_le64(const unsigned char *p)
{
    xj_payload_u64 value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (xj_payload_u64)p[i] << (8 * i);
    return value;
}

// Structural checks only. Each contained PAK must also pass its complete validator
// before publication of a Ready session. Failure always clears the output.
static inline int xj_installer_split_payloads(const void *data, xj_payload_size bytes,
                                             xj_installer_payloads *output)
{
    if (!output) return 0;
    output->ext4 = output->fat32 = 0;
    output->ext4_bytes = output->fat32_bytes = 0;
    if (!data || bytes < XJ_INSTALLER_PAYLOAD_HEADER_BYTES ||
        (__UINTPTR_TYPE__)data > ~(__UINTPTR_TYPE__)0 - bytes) return 0;
    const unsigned char *p = (const unsigned char *)data;
    const char magic[] = XJ_INSTALLER_PAYLOAD_MAGIC;
    for (unsigned i = 0; i < 8; ++i) if (p[i] != (unsigned char)magic[i]) return 0;
    if (xj_payload_le64(p + 8) != 1 || xj_payload_le64(p + 16) != 64) return 0;
    const xj_payload_u64 ext4_bytes = xj_payload_le64(p + 24);
    const xj_payload_u64 fat32_offset = xj_payload_le64(p + 32);
    const xj_payload_u64 fat32_bytes = xj_payload_le64(p + 40);
    for (unsigned i = 48; i < 64; ++i) if (p[i]) return 0;
    if (ext4_bytes < 64 || ext4_bytes > bytes - 64 || fat32_offset != 64 + ext4_bytes ||
        fat32_bytes < 16 || fat32_bytes != bytes - fat32_offset) return 0;
    const char ext4_magic[] = "XJPAK20";
    const char fat32_magic[] = "XJPAK10";
    for (unsigned i = 0; i < 8; ++i)
        if (p[64 + i] != (unsigned char)ext4_magic[i] ||
            p[fat32_offset + i] != (unsigned char)fat32_magic[i]) return 0;
    output->ext4 = p + 64;
    output->ext4_bytes = (xj_payload_size)ext4_bytes;
    output->fat32 = p + fat32_offset;
    output->fat32_bytes = (xj_payload_size)fat32_bytes;
    return 1;
}
