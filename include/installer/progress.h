#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>

namespace xjinstaller
{
enum class InstallStage : uint32_t
{
    None, Preflight, Partitioning, Formatting, Directories, Copying, Links,
    Metadata, BuildingEsp, VerifyingRoot, VerifyingEsp, Activating, Complete
};

struct InstallProgress
{
    InstallStage stage;
    uint64_t completed, total;
};

// Kernel-only callback. Capture completed work in the worker; publish only a
// bounded value snapshot. Never call back into VFS, allocate, render or wait.
struct ProgressObserver
{
    void (*update)(void *, const InstallProgress &) = nullptr;
    void *context = nullptr;
};

inline void report_progress(const ProgressObserver &observer, InstallStage stage,
                            uint64_t completed, uint64_t total)
{
    if (observer.update) observer.update(observer.context, {stage, completed, total});
}

// Weights allocate display space to real phases. They are not elapsed-time
// estimates; within a phase only completed operations advance the fraction.
inline uint32_t progress_permille(const InstallProgress &progress)
{
    constexpr uint32_t boundaries[] = {0, 0, 20, 60, 160, 190, 650, 670, 710, 750, 970, 980, 1000};
    unsigned stage = unsigned(progress.stage);
    if (stage >= unsigned(InstallStage::Complete)) return stage == unsigned(InstallStage::Complete) ? 1000 : 0;
    uint64_t done = progress.completed < progress.total ? progress.completed : progress.total;
    uint32_t begin = boundaries[stage], end = boundaries[stage + 1];
    // All current work budgets are bounded below 2^54, but keep arithmetic
    // defined for malformed snapshots as well.
    uint64_t denominator = progress.total;
    if (denominator > uint64_t(-1) / 1000) { done >>= 10; denominator >>= 10; }
    uint32_t fraction = denominator ? uint32_t(done * (end - begin) / denominator) : 0;
    uint32_t value = begin + fraction;
    return value > 999 ? 999 : value;
}
} // namespace xjinstaller
