#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

namespace installer
{
enum class EntryOperation : uint32_t
{
    legacy_start = 1,
    legacy_rescue_boot = 2,
    native_fresh = 3
};

struct WritePolicyRequest
{
    EntryOperation operation;
    uint32_t mode, language;
    uint64_t components, session_epoch;
};

struct SessionSnapshot
{
    uint64_t epoch, components;
    uint32_t profile, language;
    bool ready;
};

// Neither an old disk slot nor a worker's inherited root identity grants write
// authority. A future native request additionally needs a retained target/job
// handle. Until that service exists, every operation is refused even if all
// boot/session prerequisites are ready. No target I/O occurs in this policy.
int evaluate_write_policy(const SessionSnapshot &, const WritePolicyRequest &);
int session_write_policy(const WritePolicyRequest &);
bool session_snapshot(SessionSnapshot *);
} // namespace installer
