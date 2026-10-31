// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

// A missing/stalled hardware clock must not make polling unbounded. The poll
// fallback counts consecutive observations without clock progress, so it can
// never shorten a real timeout while the clock continues advancing.
class AhciDeadline
{
    uint64_t start;
    uint64_t last;
    uint64_t stalled;
    uint64_t stalled_limit;
    uint64_t observed;
    uint64_t polls;
    bool advanced;

public:
    explicit AhciDeadline(uint64_t now, uint64_t poll_limit = 100000000ULL)
        : start(now), last(now), stalled(0), stalled_limit(poll_limit ? poll_limit : 1), observed(now), polls(0),
          advanced(false)
    {
    }

    void restart(uint64_t now)
    {
        start = last = now;
        stalled = 0;
        observed = now;
        polls = 0;
        advanced = false;
    }

    bool expired(uint64_t now, uint64_t timeout)
    {
        observed = now;
        ++polls;
        if (now != last) advanced = true;
        if (now - start >= timeout) return true;
        if (now != last)
        {
            last = now;
            stalled = 0;
            return false;
        }
        return ++stalled >= stalled_limit;
    }

    uint64_t elapsed_ns() const { return observed - start; }
    uint64_t start_ns() const { return start; }
    uint64_t sample_ns() const { return observed; }
    uint64_t poll_count() const { return polls; }
    uint64_t stagnant_polls() const { return stalled; }
    bool clock_available() const { return start != 0 || observed != 0; }
    bool clock_progress() const { return advanced; }
};
