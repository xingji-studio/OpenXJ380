#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original XJ380 boot/root binding format. GUIDs use their GPT on-disk bytes;
// the ext4 UUID uses its raw superblock bytes. No native struct is serialized.

typedef __UINT8_TYPE__ XJ_ROOT_U8;
typedef __UINT32_TYPE__ XJ_ROOT_U32;
typedef __UINT64_TYPE__ XJ_ROOT_U64;

#define XJ_ROOT_BINDING_SIZE 96U
#define XJ_ROOT_BINDING_VERSION 1U
#define XJ_ROOT_BINDING_DISK_GUID_OFFSET 16U
#define XJ_ROOT_BINDING_ROOT_GUID_OFFSET 32U
#define XJ_ROOT_BINDING_ESP_GUID_OFFSET 48U
#define XJ_ROOT_BINDING_EXT4_UUID_OFFSET 64U
#define XJ_ROOT_BINDING_KERNEL_BYTES_OFFSET 80U
#define XJ_ROOT_BINDING_KERNEL_CRC32C_OFFSET 88U
#define XJ_ROOT_BINDING_DESCRIPTOR_CRC32C_OFFSET 92U

typedef struct XJ_ROOT_BINDING
{
    XJ_ROOT_U8 bytes[XJ_ROOT_BINDING_SIZE];
} XJ_ROOT_BINDING;

static inline XJ_ROOT_U32 xj_root_binding_le32(const XJ_ROOT_U8 *bytes)
{
    return (XJ_ROOT_U32)bytes[0] | ((XJ_ROOT_U32)bytes[1] << 8) | ((XJ_ROOT_U32)bytes[2] << 16) |
           ((XJ_ROOT_U32)bytes[3] << 24);
}

static inline XJ_ROOT_U64 xj_root_binding_le64(const XJ_ROOT_U8 *bytes)
{
    return (XJ_ROOT_U64)xj_root_binding_le32(bytes) | ((XJ_ROOT_U64)xj_root_binding_le32(bytes + 4) << 32);
}

// Reflected Castagnoli CRC, with an unfinalized state for streaming file reads.
static inline XJ_ROOT_U32 xj_root_binding_crc32c_update(XJ_ROOT_U32 state, const void *buffer, __SIZE_TYPE__ size)
{
    const XJ_ROOT_U8 *bytes = (const XJ_ROOT_U8 *)buffer;
    for (__SIZE_TYPE__ i = 0; i < size; ++i)
    {
        state ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            state = (state >> 1) ^ ((0U - (state & 1U)) & 0x82f63b78U);
    }
    return state;
}

static inline XJ_ROOT_U32 xj_root_binding_crc32c(const void *buffer, __SIZE_TYPE__ size)
{
    return xj_root_binding_crc32c_update(0xffffffffU, buffer, size) ^ 0xffffffffU;
}

static inline const XJ_ROOT_U8 *xj_root_binding_disk_guid(const XJ_ROOT_BINDING *binding)
{
    return binding->bytes + XJ_ROOT_BINDING_DISK_GUID_OFFSET;
}

static inline const XJ_ROOT_U8 *xj_root_binding_root_guid(const XJ_ROOT_BINDING *binding)
{
    return binding->bytes + XJ_ROOT_BINDING_ROOT_GUID_OFFSET;
}

static inline const XJ_ROOT_U8 *xj_root_binding_esp_guid(const XJ_ROOT_BINDING *binding)
{
    return binding->bytes + XJ_ROOT_BINDING_ESP_GUID_OFFSET;
}

static inline const XJ_ROOT_U8 *xj_root_binding_ext4_uuid(const XJ_ROOT_BINDING *binding)
{
    return binding->bytes + XJ_ROOT_BINDING_EXT4_UUID_OFFSET;
}

static inline XJ_ROOT_U64 xj_root_binding_kernel_bytes(const XJ_ROOT_BINDING *binding)
{
    return xj_root_binding_le64(binding->bytes + XJ_ROOT_BINDING_KERNEL_BYTES_OFFSET);
}

static inline XJ_ROOT_U32 xj_root_binding_kernel_crc32c(const XJ_ROOT_BINDING *binding)
{
    return xj_root_binding_le32(binding->bytes + XJ_ROOT_BINDING_KERNEL_CRC32C_OFFSET);
}

static inline int xj_root_binding_valid(const XJ_ROOT_BINDING *binding)
{
    static const XJ_ROOT_U8 magic[8] = {'X', 'J', 'R', 'O', 'O', 'T', '0', '1'};
    if (!binding) return 0;
    for (unsigned i = 0; i < sizeof(magic); ++i)
        if (binding->bytes[i] != magic[i]) return 0;
    if (xj_root_binding_le32(binding->bytes + 8) != XJ_ROOT_BINDING_VERSION ||
        xj_root_binding_le32(binding->bytes + 12) != XJ_ROOT_BINDING_SIZE ||
        xj_root_binding_kernel_bytes(binding) == 0)
        return 0;
    return xj_root_binding_crc32c(binding->bytes, XJ_ROOT_BINDING_DESCRIPTOR_CRC32C_OFFSET) ==
           xj_root_binding_le32(binding->bytes + XJ_ROOT_BINDING_DESCRIPTOR_CRC32C_OFFSET);
}

static inline int xj_root_binding_kernel_matches(const XJ_ROOT_BINDING *binding, const void *kernel,
                                               __SIZE_TYPE__ kernel_size)
{
    return xj_root_binding_valid(binding) && kernel &&
           xj_root_binding_kernel_bytes(binding) == (XJ_ROOT_U64)kernel_size &&
           xj_root_binding_kernel_crc32c(binding) == xj_root_binding_crc32c(kernel, kernel_size);
}
