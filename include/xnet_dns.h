// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "xnet.h"
#ifdef __cplusplus
extern "C" {
#endif
XNetResult xapi_NetResolveStart(const XNetResolveDesc *desc, XNetHandle *query);
// NULL buffer/capacity 0 queries count. Short capacity: -ENOBUFS + required.
// Pending: -EINPROGRESS. Result is stable until NetClose(query).
XNetResult xapi_NetResolveGetResult(XNetHandle query, XNetEndpoint *endpoints, uint32_t capacity, uint32_t *required);
#ifdef __cplusplus
}
#endif
