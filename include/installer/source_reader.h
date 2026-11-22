#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <installer/source_lifetime.h>
#include <installer/stream_index.h>

namespace installer {
// Kernel-owned, zero-initialized and retained to reboot. The two references
// become immutable before publication; a single admitted worker reads at once.
struct SourceReadContext
{
    SourceLifetime                 *source    = nullptr;
    const xjinstaller::StreamIndex *index     = nullptr;
    xjpak2::DataReader              files     = {};
    bool                            busy      = false;
    bool                            preparing = false;
    alignas(64) uint8_t bounce[65536];
};

// Source-only exact reader, with real hardware read-only and generation checks.
// No filesystem mount, target write, unbounded allocation or firmware callback.
int read_stream_source(void *, uint64_t offset, void *, xjpak2::Size);
// Called once by the boot owner after source retention, before publication.
int prepare_stream_source(SourceReadContext &);
// Physical-byte adapter for the private file reader. Caller owns reader.busy,
// or is the unpublished boot owner; no nested acquisition uses its scratch.
int read_source_device(void *, uint64_t physical, void *, xjpak2::Size);
} // namespace installer
