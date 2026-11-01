// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "xnet.h"

#define XNET_SAVED_ENABLED       1U
#define XNET_SAVED_RECOVERED     2U
#define XNET_CONFIG_DONE         1U
#define XNET_CONFIG_ENABLED      2U
#define XNET_CONFIG_RECOVERED    4U
#define XNET_CONFIG_APPLIED      8U
#define XNET_CONFIG_ANY_REVISION (~0ULL)

// One persistent policy for the default logical interface (ID 0 or 1).
// revision is an optimistic write guard. ANY_REVISION explicitly replaces even
// corrupt snapshots; normal callers round-trip the revision returned by Read.
typedef struct XNetSavedConfig
{
    uint32_t            size, version, flags, reserved;
    uint64_t            revision;
    XNetInterfaceConfig ipv4;
    XNetDnsConfig       dns;
    uint64_t            reserved64[2];
} XNetSavedConfig;

// Last boot/explicit reload attempt; runtime-only Apply operations do not
// update this history. Query runtime state separately. DONE includes failures.
typedef struct XNetConfigStatus
{
    uint32_t size, version, flags, reserved;
    uint64_t revision;
    int64_t  load_result, apply_result;
    uint64_t reserved64[2];
} XNetConfigStatus;
#ifdef __cplusplus
extern "C" {
#endif
XNetResult xapi_NetEnumInterfaces(XNetInterfaceId *interfaces, uint32_t capacity, uint32_t *required);
XNetResult xapi_NetGetInterfaceInfo(XNetInterfaceId interface_id, XNetInterfaceInfo *info);
XNetResult xapi_NetGetInterfaceAddresses(XNetInterfaceId interface_id, XNetInterfaceAddress *addresses,
                                         uint32_t capacity, uint32_t *required);
XNetResult xapi_NetGetDnsConfig(XNetDnsConfig *config);
// Runtime only. ChangeSettings is checked inside the shared kernel boundary.
XNetResult xapi_NetApplyInterfaceConfig(const XNetInterfaceConfig *config);
XNetResult xapi_NetApplyDnsConfig(const XNetDnsConfig *config);
// Missing storage returns a disabled DHCP/automatic-DNS template, revision 0.
// Save requires CS, copies input and changes storage only. Disabled saves are
// mirrored tombstones, so losing either slot cannot revive an enabled snapshot.
XNetResult xapi_NetGetSavedConfig(XNetSavedConfig *config);
XNetResult xapi_NetSaveConfig(const XNetSavedConfig *config);
// CS required. Fully validate before applying. Disabled/missing: no override.
// If backend application fails, query runtime state; combined apply is not atomic.
XNetResult xapi_NetReloadConfig(void);
XNetResult xapi_NetGetConfigStatus(XNetConfigStatus *status);
// V1 coalesces changes; RESYNC means re-read the full state. interface_id 0=all.
XNetResult xapi_NetWatchCreate(XNetInterfaceId interface_id, XNetHandle *watch);
XNetResult xapi_NetWatchRead(XNetHandle watch, XNetChange *change);
#ifdef __cplusplus
}
#endif
