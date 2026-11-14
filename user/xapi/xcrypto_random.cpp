// SPDX-License-Identifier: Apache-2.0
#include "include/errno.h"
#include "include/libsys.h"
#include "include/xnet_runtime.h"

namespace
{
int random_bytes(void *output, uint32_t bytes, uint64_t flags)
{
    if (bytes && !output) return -EINVAL;
    auto *data = static_cast<uint8_t *>(output);
    uint32_t offset = 0;
    while (offset < bytes)
    {
        // Existing getrandom ABI; all CPU/device selection belongs to the kernel.
        int64_t got = (int64_t)enter_syscall(318, (uint64_t)(data + offset), bytes - offset, flags, 0, 0, 0);
        if (got <= 0 || (uint64_t)got > bytes - offset)
        {
            volatile uint8_t *erase = data;
            for (uint32_t i = 0; i < bytes; ++i) erase[i] = 0;
            return got < 0 ? int(got) : -EIO;
        }
        offset += uint32_t(got);
    }
    return 0;
}
}

extern "C" int xapi_secure_random(void *output, uint32_t bytes)
{
    return random_bytes(output, bytes, 0);
}

extern "C" int xapi_random_bytes(void *output, uint32_t bytes)
{
    return random_bytes(output, bytes, 4); // GRND_INSECURE, ordinary non-secret uses only.
}
