#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

namespace xjext4 {

enum AttributeMask : uint32_t
{
    kAttributeMode = 1U,
    kAttributeUid = 2U,
    kAttributeGid = 4U,
    kAttributeAtime = 8U,
    kAttributeMtime = 16U,
};

struct AttributeUpdate
{
    uint32_t mask;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    int64_t atime_seconds;
    int64_t mtime_seconds;
    uint32_t atime_nanoseconds;
    uint32_t mtime_nanoseconds;
};

} // namespace xjext4
