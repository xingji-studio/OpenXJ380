#include "../xapi/include/stdint.h"

#define SXAH_INSTALLER_ENUM_DISKS       128956723895689220
#define SXAH_INSTALLER_START            128956723895689221
#define SXAH_INSTALLER_PROGRESS         128956723895689222
#define SXAH_INSTALLER_PRECHECK         128956723895689223
#define SXAH_INSTALLER_START_EX         128956723895689224
#define SXAH_INSTALLER_RESCUE           128956723895689225
#define SXAH_INSTALLER_LOG              128956723895689226
#define SXAH_INSTALLER_START_OPTIONS    128956723895689227
#define SXAH_INSTALLER_PRECHECK_OPTIONS 128956723895689228

extern "C" uint64_t enter_syscall(uint64_t syscall_number, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                  uint64_t arg4, uint64_t arg5, uint64_t arg6);

extern "C" UINT64 xapi_InstallerEnumDisks(void *list)
{
    return enter_syscall(SXAH_INSTALLER_ENUM_DISKS, (uint64_t)list, 0, 0, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerStart(UINT64 disk_id)
{
    return enter_syscall(SXAH_INSTALLER_START, disk_id, 0, 0, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerStartEx(UINT64 disk_id, UINT64 mode)
{
    return enter_syscall(SXAH_INSTALLER_START_EX, disk_id, mode, 0, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerStartOptions(void *options)
{
    return enter_syscall(SXAH_INSTALLER_START_OPTIONS, (uint64_t)options, 0, 0, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerPrecheck(UINT64 disk_id, UINT64 mode, void *out)
{
    return enter_syscall(SXAH_INSTALLER_PRECHECK, disk_id, mode, (uint64_t)out, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerPrecheckOptions(void *options, void *out)
{
    return enter_syscall(SXAH_INSTALLER_PRECHECK_OPTIONS, (uint64_t)options, (uint64_t)out, 0, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerProgress(void *progress)
{
    return enter_syscall(SXAH_INSTALLER_PROGRESS, (uint64_t)progress, 0, 0, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerRescue(UINT64 action, UINT64 disk_id, void *out)
{
    return enter_syscall(SXAH_INSTALLER_RESCUE, action, disk_id, (uint64_t)out, 0, 0, 0);
}

extern "C" UINT64 xapi_InstallerLog(void *out)
{
    return enter_syscall(SXAH_INSTALLER_LOG, (uint64_t)out, 0, 0, 0, 0, 0);
}
