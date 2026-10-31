#pragma once

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
