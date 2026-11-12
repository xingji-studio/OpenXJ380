#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <installer/source_lifetime.h>
#include <installer/stream_index.h>

namespace installer
{
// Kernel-owned, zero-initialized and retained to reboot. The two references
// become immutable before publication; a single admitted worker reads at once.
struct SourceReadContext
{
    SourceLifetime *source = nullptr;
    const xjinstaller::StreamIndex *index = nullptr;
    bool busy = false;
    alignas(64) uint8_t bounce[65536];
};

// Source-only exact reader, with real hardware read-only and generation checks.
// No filesystem mount, target write, unbounded allocation or firmware callback.
int read_stream_source(void *, uint64_t offset, void *, xjpak2::Size);
} // namespace installer
