#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original XJ380 installer handoff. No firmware handles survive ExitBootServices.
// This is an opt-in in-memory ABI, independent of the 96-byte root binding.

typedef __UINT8_TYPE__ XJ_IH_U8;
typedef __UINT16_TYPE__ XJ_IH_U16;
typedef __UINT32_TYPE__ XJ_IH_U32;
typedef __UINT64_TYPE__ XJ_IH_U64;

#define XJ_INSTALLER_HANDOFF_MAGIC 0x48494a58U
#define XJ_INSTALLER_HANDOFF_VERSION 1U
#define XJ_INSTALLER_PCI_MAX 16U
#define XJ_INSTALLER_PATH_MAX 512U
#define XJ_INSTALLER_PACKAGE_MAX (2ULL * 1024 * 1024 * 1024)
#define XJ_INSTALLER_PACKAGES_MAX (3ULL * 1024 * 1024 * 1024)
#define XJ_INSTALLER_SOURCE_SUPPORTED 1U
#define XJ_INSTALLER_SOURCE_UNSUPPORTED 2U
#define XJ_INSTALLER_SOURCE_MALFORMED 3U
#define XJ_INSTALLER_SOURCE_AMBIGUOUS 4U
#define XJ_INSTALLER_SOURCE_AHCI 1U
#define XJ_INSTALLER_SOURCE_NVME 2U
#define XJ_INSTALLER_SOURCE_IDE 3U
#define XJ_INSTALLER_SOURCE_READ_ONLY 1U
#define XJ_INSTALLER_VOLUME_WHOLE 1U
#define XJ_INSTALLER_VOLUME_HD 2U
#define XJ_INSTALLER_VOLUME_CD 3U
#define XJ_INSTALLER_FORMAT_PAK1 1U
#define XJ_INSTALLER_FORMAT_PAK2 2U
#define XJ_INSTALLER_PROFILE_FAT32_PAK1 1U
#define XJ_INSTALLER_PROFILE_EXT4_PAK2 2U

typedef struct XJ_INSTALLER_PCI_LOCATION
{
    XJ_IH_U16 segment;
    XJ_IH_U8 bus, device, function;
    XJ_IH_U8 reserved[3];
} XJ_INSTALLER_PCI_LOCATION;

typedef struct XJ_INSTALLER_PACKAGE_SPAN
{
    XJ_IH_U64 physical;
    XJ_IH_U64 bytes;
    XJ_IH_U64 pages;
} XJ_INSTALLER_PACKAGE_SPAN;

typedef struct XJ_INSTALLER_SOURCE
{
    XJ_IH_U32 status;
    XJ_IH_U32 transport;
    XJ_IH_U32 flags;
    XJ_IH_U32 pci_count;
    XJ_IH_U32 physical_block_size;
    XJ_IH_U32 volume_block_size;
    XJ_IH_U64 physical_bytes;
    XJ_IH_U64 volume_offset;
    XJ_IH_U64 volume_bytes;
    XJ_IH_U16 sata_port, sata_multiplier, lun;
    XJ_IH_U8 ide_channel, ide_unit;
    XJ_IH_U32 nvme_namespace;
    XJ_IH_U8 nvme_eui64[8];
    XJ_IH_U32 volume_kind;
    XJ_IH_U32 volume_index;
    XJ_IH_U32 path_bytes;
    XJ_IH_U32 controller_path_bytes;
    XJ_IH_U32 reserved0;
    XJ_IH_U64 reserved[4];
    XJ_INSTALLER_PCI_LOCATION pci[XJ_INSTALLER_PCI_MAX];
    XJ_IH_U8 path[XJ_INSTALLER_PATH_MAX];
} XJ_INSTALLER_SOURCE;

typedef struct XJ_INSTALLER_HANDOFF
{
    XJ_IH_U32 magic;
    XJ_IH_U32 version;
    XJ_IH_U32 bytes;
    XJ_IH_U32 root_format;
    XJ_IH_U32 system_profile;
    XJ_IH_U32 reserved0;
    XJ_INSTALLER_PACKAGE_SPAN root;
    XJ_INSTALLER_PACKAGE_SPAN system;
    XJ_INSTALLER_SOURCE source;
    XJ_IH_U64 loader_image_physical;
    XJ_IH_U64 loader_image_bytes;
    XJ_IH_U64 reserved[4];
} XJ_INSTALLER_HANDOFF;

static inline void xj_installer_handoff_zero(void *object, __SIZE_TYPE__ bytes)
{
    XJ_IH_U8 *p = (XJ_IH_U8 *)object;
    for (__SIZE_TYPE__ i = 0; i < bytes; ++i) p[i] = 0;
}

static inline XJ_IH_U16 xj_installer_le16(const XJ_IH_U8 *p)
{
    return (XJ_IH_U16)((XJ_IH_U16)p[0] | ((XJ_IH_U16)p[1] << 8));
}

static inline XJ_IH_U32 xj_installer_le32(const XJ_IH_U8 *p)
{
    return (XJ_IH_U32)xj_installer_le16(p) | ((XJ_IH_U32)xj_installer_le16(p + 2) << 16);
}

static inline XJ_IH_U64 xj_installer_le64(const XJ_IH_U8 *p)
{
    return (XJ_IH_U64)xj_installer_le32(p) | ((XJ_IH_U64)xj_installer_le32(p + 4) << 32);
}

static inline int xj_installer_bytes_zero(const void *object, __SIZE_TYPE__ bytes)
{
    const XJ_IH_U8 *p = (const XJ_IH_U8 *)object;
    for (__SIZE_TYPE__ i = 0; i < bytes; ++i) if (p[i]) return 0;
    return 1;
}

// Arithmetic/ownership descriptor checks only. The kernel must additionally
// authenticate final MemoryMap coverage, actual mappings and disjoint owners.
static inline int xj_installer_package_span_valid(const XJ_INSTALLER_PACKAGE_SPAN *span)
{
    return span && span->physical && !(span->physical & 4095) && span->bytes &&
           span->bytes <= XJ_INSTALLER_PACKAGE_MAX && span->pages == (span->bytes + 4095) / 4096 &&
           span->physical <= (~(XJ_IH_U64)0) - span->pages * 4096;
}

// A deliberately narrow single-instance UEFI profile. The parser reads packed
// nodes by bytes; unknown transports, vendor/RAID nodes and nested media fail
// closed. PCI segment/bus comes from each actual firmware PCI_IO GetLocation,
// never from the ACPI root UID or a guessed bus number.
typedef struct XJ_INSTALLER_PATH_INFO
{
    XJ_INSTALLER_SOURCE identity;
    XJ_IH_U32 pci_end[XJ_INSTALLER_PCI_MAX];
    XJ_IH_U32 physical_path_bytes;
    XJ_IH_U64 media_start;
    XJ_IH_U64 media_blocks;
} XJ_INSTALLER_PATH_INFO;

static inline XJ_IH_U32 xj_installer_parse_device_path(const XJ_IH_U8 *path, __SIZE_TYPE__ limit,
                                                     XJ_INSTALLER_PATH_INFO *info)
{
    if (!info) return XJ_INSTALLER_SOURCE_MALFORMED;
    xj_installer_handoff_zero(info, sizeof(*info));
    if (!path) return XJ_INSTALLER_SOURCE_MALFORMED;
    if (limit > XJ_INSTALLER_PATH_MAX) limit = XJ_INSTALLER_PATH_MAX;
    XJ_INSTALLER_SOURCE *s = &info->identity;
    __SIZE_TYPE__ offset = 0;
    int root_seen = 0;
    while (offset <= limit && limit - offset >= 4)
    {
        const XJ_IH_U8 *n = path + offset;
        XJ_IH_U16 bytes = xj_installer_le16(n + 2);
        if (bytes < 4 || bytes > limit - offset) return XJ_INSTALLER_SOURCE_MALFORMED;
        if (n[0] == 0x7f)
        {
            if (n[1] != 0xff) return XJ_INSTALLER_SOURCE_UNSUPPORTED;
            if (bytes != 4 || !root_seen || !s->transport || !s->pci_count)
                return XJ_INSTALLER_SOURCE_MALFORMED;
            s->path_bytes = (XJ_IH_U32)(offset + 4);
            if (!s->volume_kind) s->volume_kind = XJ_INSTALLER_VOLUME_WHOLE;
            for (__SIZE_TYPE__ i = 0; i < s->path_bytes; ++i) s->path[i] = path[i];
            s->status = XJ_INSTALLER_SOURCE_SUPPORTED;
            return s->status;
        }
        if (!root_seen)
        {
            if (n[0] != 2 || n[1] != 1 || bytes != 12 ||
                (xj_installer_le32(n + 4) != 0x0a0341d0U &&
                 xj_installer_le32(n + 4) != 0x0a0841d0U)) return XJ_INSTALLER_SOURCE_UNSUPPORTED;
            root_seen = 1;
        }
        else if (n[0] == 1 && n[1] == 1)
        {
            if (bytes != 6 || s->transport || s->pci_count == XJ_INSTALLER_PCI_MAX || n[4] > 7 || n[5] > 31)
                return XJ_INSTALLER_SOURCE_MALFORMED;
            XJ_IH_U32 i = s->pci_count++;
            s->pci[i].function = n[4];
            s->pci[i].device = n[5];
            info->pci_end[i] = (XJ_IH_U32)(offset + bytes);
        }
        else if (n[0] == 3)
        {
            if (!s->pci_count || s->transport || s->volume_kind) return XJ_INSTALLER_SOURCE_MALFORMED;
            s->controller_path_bytes = (XJ_IH_U32)offset;
            if (n[1] == 0x12 && bytes == 10)
            {
                s->transport = XJ_INSTALLER_SOURCE_AHCI;
                s->sata_port = xj_installer_le16(n + 4);
                s->sata_multiplier = xj_installer_le16(n + 6);
                s->lun = xj_installer_le16(n + 8);
                if (s->sata_port >= 32 || s->sata_multiplier != 0xffff || s->lun)
                    return XJ_INSTALLER_SOURCE_UNSUPPORTED;
            }
            else if (n[1] == 0x17 && bytes == 16)
            {
                s->transport = XJ_INSTALLER_SOURCE_NVME;
                s->nvme_namespace = xj_installer_le32(n + 4);
                if (!s->nvme_namespace || s->nvme_namespace == 0xffffffffU)
                    return XJ_INSTALLER_SOURCE_MALFORMED;
                for (unsigned i = 0; i < 8; ++i) s->nvme_eui64[i] = n[8 + i];
            }
            else if (n[1] == 1 && bytes == 8)
            {
                s->transport = XJ_INSTALLER_SOURCE_IDE;
                s->ide_channel = n[4];
                s->ide_unit = n[5];
                s->lun = xj_installer_le16(n + 6);
                if (s->ide_channel > 1 || s->ide_unit > 1 || s->lun)
                    return XJ_INSTALLER_SOURCE_UNSUPPORTED;
            }
            else return XJ_INSTALLER_SOURCE_UNSUPPORTED;
            info->physical_path_bytes = (XJ_IH_U32)(offset + bytes);
        }
        else if (n[0] == 4)
        {
            if (!s->transport || s->volume_kind) return XJ_INSTALLER_SOURCE_UNSUPPORTED;
            if (n[1] == 1 && bytes == 42)
            {
                if (!xj_installer_le32(n + 4) || (n[40] != 1 && n[40] != 2) ||
                    (n[41] != 1 && n[41] != 2)) return XJ_INSTALLER_SOURCE_MALFORMED;
                s->volume_kind = XJ_INSTALLER_VOLUME_HD;
                s->volume_index = xj_installer_le32(n + 4);
                info->media_start = xj_installer_le64(n + 8);
                info->media_blocks = xj_installer_le64(n + 16);
            }
            else if (n[1] == 2 && bytes == 24)
            {
                s->volume_kind = XJ_INSTALLER_VOLUME_CD;
                s->volume_index = xj_installer_le32(n + 4);
                info->media_start = xj_installer_le64(n + 8);
                info->media_blocks = xj_installer_le64(n + 16);
            }
            else return XJ_INSTALLER_SOURCE_UNSUPPORTED;
            if (!info->media_start || !info->media_blocks) return XJ_INSTALLER_SOURCE_MALFORMED;
        }
        else return XJ_INSTALLER_SOURCE_UNSUPPORTED;
        offset += bytes;
    }
    return XJ_INSTALLER_SOURCE_MALFORMED;
}

// Reparse the captured actual LoadedImage volume path and compare every
// normalized field. Geometry and segment/bus are independently captured from
// firmware protocols; the kernel sidecar authenticates those against drivers.
static inline int xj_installer_source_valid(const XJ_INSTALLER_SOURCE *s)
{
    if (!s || s->status != XJ_INSTALLER_SOURCE_SUPPORTED || s->reserved0 ||
        (s->flags & ~XJ_INSTALLER_SOURCE_READ_ONLY) || s->pci_count > XJ_INSTALLER_PCI_MAX ||
        !s->pci_count || !s->physical_block_size || !s->volume_block_size ||
        (s->physical_block_size & (s->physical_block_size - 1)) ||
        (s->volume_block_size & (s->volume_block_size - 1)) ||
        s->physical_block_size < 512 || s->physical_block_size > 65536 ||
        s->volume_block_size < 512 || s->volume_block_size > 65536 ||
        !s->physical_bytes || s->physical_bytes % s->physical_block_size || !s->volume_bytes ||
        s->volume_bytes % s->volume_block_size || s->volume_offset > s->physical_bytes ||
        s->volume_bytes > s->physical_bytes - s->volume_offset ||
        s->path_bytes < 4 || s->path_bytes > XJ_INSTALLER_PATH_MAX ||
        !xj_installer_bytes_zero(s->reserved, sizeof(s->reserved)) ||
        !xj_installer_bytes_zero(s->path + s->path_bytes, XJ_INSTALLER_PATH_MAX - s->path_bytes)) return 0;
    XJ_INSTALLER_PATH_INFO info;
    if (xj_installer_parse_device_path(s->path, s->path_bytes, &info) != XJ_INSTALLER_SOURCE_SUPPORTED) return 0;
    const XJ_INSTALLER_SOURCE *p = &info.identity;
    if (p->path_bytes != s->path_bytes || p->controller_path_bytes != s->controller_path_bytes ||
        p->pci_count != s->pci_count || p->transport != s->transport || p->sata_port != s->sata_port ||
        p->sata_multiplier != s->sata_multiplier || p->lun != s->lun || p->ide_channel != s->ide_channel ||
        p->ide_unit != s->ide_unit || p->nvme_namespace != s->nvme_namespace ||
        p->volume_kind != s->volume_kind || p->volume_index != s->volume_index) return 0;
    for (unsigned i = 0; i < 8; ++i) if (p->nvme_eui64[i] != s->nvme_eui64[i]) return 0;
    for (unsigned i = 0; i < s->pci_count; ++i)
    {
        if (s->pci[i].device != p->pci[i].device || s->pci[i].function != p->pci[i].function ||
            s->pci[i].segment != s->pci[0].segment ||
            !xj_installer_bytes_zero(s->pci[i].reserved, sizeof(s->pci[i].reserved))) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (s->pci[i].bus == s->pci[j].bus) return 0;
    }
    if (!xj_installer_bytes_zero(s->pci + s->pci_count,
                                sizeof(s->pci) - s->pci_count * sizeof(s->pci[0]))) return 0;
    if (s->volume_kind == XJ_INSTALLER_VOLUME_WHOLE)
        return !s->volume_offset && s->volume_bytes == s->physical_bytes &&
               s->volume_block_size == s->physical_block_size;
    if (info.media_start > (~(XJ_IH_U64)0) / s->physical_block_size ||
        info.media_blocks > (~(XJ_IH_U64)0) / s->physical_block_size) return 0;
    return s->volume_offset == info.media_start * s->physical_block_size &&
           s->volume_bytes == info.media_blocks * s->physical_block_size;
}

static inline int xj_installer_handoff_valid(const XJ_INSTALLER_HANDOFF *handoff)
{
    if (!handoff || handoff->magic != XJ_INSTALLER_HANDOFF_MAGIC ||
        handoff->version != XJ_INSTALLER_HANDOFF_VERSION || handoff->bytes != sizeof(*handoff) ||
        handoff->root_format != XJ_INSTALLER_FORMAT_PAK1 || handoff->reserved0 ||
        !handoff->loader_image_physical || !handoff->loader_image_bytes ||
        handoff->loader_image_physical > (~(XJ_IH_U64)0) - handoff->loader_image_bytes ||
        (handoff->system_profile != XJ_INSTALLER_PROFILE_FAT32_PAK1 &&
         handoff->system_profile != XJ_INSTALLER_PROFILE_EXT4_PAK2) ||
        !xj_installer_bytes_zero(handoff->reserved, sizeof(handoff->reserved)) ||
        !xj_installer_package_span_valid(&handoff->root) ||
        !xj_installer_package_span_valid(&handoff->system) ||
        handoff->root.bytes + handoff->system.bytes > XJ_INSTALLER_PACKAGES_MAX) return 0;
    if (handoff->source.status == XJ_INSTALLER_SOURCE_SUPPORTED)
    {
        if (!xj_installer_source_valid(&handoff->source)) return 0;
    }
    else
    {
        if (handoff->source.status < XJ_INSTALLER_SOURCE_UNSUPPORTED ||
            handoff->source.status > XJ_INSTALLER_SOURCE_AMBIGUOUS ||
            !xj_installer_bytes_zero((const XJ_IH_U8 *)&handoff->source + sizeof(handoff->source.status),
                                     sizeof(handoff->source) - sizeof(handoff->source.status))) return 0;
    }
    XJ_IH_U64 root_end = handoff->root.physical + handoff->root.pages * 4096;
    XJ_IH_U64 system_end = handoff->system.physical + handoff->system.pages * 4096;
    return root_end <= handoff->system.physical || system_end <= handoff->root.physical;
}


#if defined(__cplusplus)
static_assert(sizeof(XJ_INSTALLER_PCI_LOCATION) == 8, "installer PCI location ABI");
static_assert(sizeof(XJ_INSTALLER_SOURCE) == 760, "installer source version 1 ABI");
static_assert(sizeof(XJ_INSTALLER_HANDOFF) == 880, "installer handoff version 1 ABI");
#else
_Static_assert(sizeof(XJ_INSTALLER_PCI_LOCATION) == 8, "installer PCI location ABI");
_Static_assert(sizeof(XJ_INSTALLER_SOURCE) == 760, "installer source version 1 ABI");
_Static_assert(sizeof(XJ_INSTALLER_HANDOFF) == 880, "installer handoff version 1 ABI");
#endif
