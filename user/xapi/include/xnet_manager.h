// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "xnet.h"
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
// V1 coalesces changes; RESYNC means re-read the full state. interface_id 0=all.
XNetResult xapi_NetWatchCreate(XNetInterfaceId interface_id, XNetHandle *watch);
XNetResult xapi_NetWatchRead(XNetHandle watch, XNetChange *change);
#ifdef __cplusplus
}
#endif
