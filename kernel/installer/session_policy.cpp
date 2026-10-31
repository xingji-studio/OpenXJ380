// SPDX-License-Identifier: Apache-2.0

#include <installer/session.h>

namespace installer
{
int evaluate_write_policy(const SessionSnapshot &session, const WritePolicyRequest &request)
{
    constexpr int no_device = -19;
    constexpr int unsupported = -95;
    constexpr int stale = -116;
    if (!session.ready || !session.epoch) return no_device;
    switch (request.operation)
    {
    case EntryOperation::legacy_start:
    case EntryOperation::legacy_rescue_boot:
        return unsupported;
    case EntryOperation::native_fresh:
        if (request.session_epoch != session.epoch) return stale;
        // A session proves the source and package lifetime, not authorization
        // of an arbitrary target. Do not publish the native factory yet.
        return unsupported;
    default:
        return unsupported;
    }
}

int session_write_policy(const WritePolicyRequest &request)
{
    SessionSnapshot snapshot = {};
    session_snapshot(&snapshot);
    return evaluate_write_policy(snapshot, request);
}
} // namespace installer
