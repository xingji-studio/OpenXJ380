#pragma once

#include "../xapi/include/stdint.h"

extern "C" UINT64 xapi_InstallerEnumDisks(void *list);
extern "C" UINT64 xapi_InstallerStart(UINT64 disk_id);
extern "C" UINT64 xapi_InstallerStartEx(UINT64 disk_id, UINT64 mode);
extern "C" UINT64 xapi_InstallerStartOptions(void *options);
extern "C" UINT64 xapi_InstallerPrecheck(UINT64 disk_id, UINT64 mode, void *out);
extern "C" UINT64 xapi_InstallerPrecheckOptions(void *options, void *out);
extern "C" UINT64 xapi_InstallerProgress(void *progress);
extern "C" UINT64 xapi_InstallerRescue(UINT64 action, UINT64 disk_id, void *out);
extern "C" UINT64 xapi_InstallerLog(void *out);
