#pragma once

#include "stdint.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

typedef enum
{
    XUT_Root,
    XUT_System,
    XUT_Admin,
    XUT_Visitor,
    XUT_Custom
} UserType;

typedef struct
{
    bool ReadFile;
    bool WriteFile;
    bool ChangeSettings;
    bool VisitAnotherDir;
    bool VisitSysDir;
    bool ReadSysFile;
    bool WriteSysFile;
} UserPermisson;

// Failure masks are explicitly packed; do not use compiler-dependent bitfields.
enum
{
    XPERM_R = 0x40, XPERM_W = 0x20, XPERM_CS = 0x10, XPERM_NV = 0x08,
    XPERM_SV = 0x04, XPERM_SR = 0x02, XPERM_SW = 0x01, XPERM_ALL = 0x7f
};

static inline unsigned char user_permission_mask(UserPermisson p)
{
    return (p.ReadFile ? XPERM_R : 0) | (p.WriteFile ? XPERM_W : 0) |
           (p.ChangeSettings ? XPERM_CS : 0) | (p.VisitAnotherDir ? XPERM_NV : 0) |
           (p.VisitSysDir ? XPERM_SV : 0) | (p.ReadSysFile ? XPERM_SR : 0) |
           (p.WriteSysFile ? XPERM_SW : 0);
}

static inline UserPermisson user_permissions_from_mask(unsigned char m)
{
    UserPermisson p = {(m & XPERM_R) != 0, (m & XPERM_W) != 0, (m & XPERM_CS) != 0,
                      (m & XPERM_NV) != 0, (m & XPERM_SV) != 0, (m & XPERM_SR) != 0,
                      (m & XPERM_SW) != 0};
    return p;
}

static inline unsigned char user_type_permission_mask(UserType type)
{
    switch (type)
    {
    case XUT_Root:
    case XUT_System: return XPERM_ALL;
    case XUT_Admin: return XPERM_R | XPERM_W | XPERM_CS | XPERM_NV | XPERM_SV;
    // The CSV's Visitor NV=Y/N requires explicit consent, so it starts disabled.
    case XUT_Visitor: return XPERM_R | XPERM_W;
    default: return 0;
    }
}

// Special permissions override ordinary R/W/CS/NV/SV/SR/SW for the
// current account's /users/<username>/picture directory and its descendants.
typedef enum
{
    XSPT_Zero = 0,         // Reserved; never a valid request.
    XSPT_ReadPicture = 1,  // List, inspect and read /users/<username>/picture.
    XSPT_WritePicture = 2, // Create and write album contents; does not authorize deletion.
    XSPT_DeletePicture = 3 // Remove album entries; does not authorize reading or writing.
} SpecialPermissionType;

enum
{
    XSP_STORE_MAGIC = 0x31505358, // XSP1, little endian.
    XSP_STORE_VERSION = 1,
    XSP_STORE_COMMITTED = 0x58434d54, // Written last; each byte is nonzero.
    XSP_APP_PATH_BYTES = 4096,
    XSP_MAX_APPS = 256,
    XSP_STORE_LIMIT = 1024 * 1024 + 65536
};
#define XSP_KNOWN_MASK ((1ULL << XSPT_ReadPicture) | (1ULL << XSPT_WritePicture) | \
                        (1ULL << XSPT_DeletePicture))

// /system/config/user/<username>/permission.dat, with a verified .backup
// snapshot for interrupted writes. Fixed-width, little-endian disk ABI.
// The header is followed by SpAppCount variable-length application records.
// Checksum is FNV-1a over the whole snapshot with Checksum set to zero.
// Commit must equal XSP_STORE_COMMITTED; incomplete writes are not decisions.
typedef struct
{
    uint32_t Magic, Version, SpAppCount, RecordsBytes;
    uint64_t Generation;
    uint32_t Checksum, Commit;
    char UserName[64];
} SpecialPremissionSettingFileFormat;
typedef SpecialPremissionSettingFileFormat SpecialPermissionSettingFileFormat;

// Each record contains this header followed by PathBytes bytes of a canonical
// executable pathname, including its NUL. RecordBytes = sizeof(header) + PathBytes.
// Records have no padding and must be decoded with memcpy (may be unaligned).
// Decided records both approval and refusal; Granted is a subset of Decided.
// An existing decision is returned without another consent dialog on later runs.
typedef struct
{
    uint32_t RecordBytes, PathBytes;
    uint64_t Decided, Granted;
} SpecialPermissionAppRecord;
