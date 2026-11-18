#pragma once
// SPDX-License-Identifier: Apache-2.0
// Optional caller-owned measurements. No allocator, kernel clock or I/O dependency.
#include <stdint.h>

namespace xjext4 {

struct PerformanceMetric
{
    uint64_t calls;
    uint64_t nanoseconds;
};

// Scopes are inclusive and overlap: do not sum their times. Counters record
// attempts except fields explicitly named committed/succeeded. Successful writes
// may still be volatile; a failed write/flush can have persisted an unknown prefix.
// The exclusive session owner keeps this object and its nonblocking monotonic
// clock alive through close or quarantine. A null clock retains counts only.
// Value-initialize with {} or use zero-filled storage; keep these types trivial.
struct Performance
{
    uint64_t (*now)(void *);
    void *context;
    PerformanceMetric create, pwrite, attributes, commit, validation, layout_validation, small_create;
    PerformanceMetric log_write, checkpoint_write, flush, backend_read;
    // The importer measures all target callbacks, including recovery-flag I/O
    // outside Journal. flush/device_write above and below are journal-only.
    PerformanceMetric target_read, target_write, target_flush;
    uint64_t target_read_bytes;
    uint64_t target_write_bytes;
    uint64_t transactions_committed;
    uint64_t blocks_committed;
    uint64_t capacity_committed;
    uint64_t peak_transaction_blocks;
    uint64_t small_batches_committed;
    uint64_t small_files_committed;
    uint64_t attribute_requests;
    uint64_t attributes_committed;
    uint64_t payload_bytes_committed;
    uint64_t log_data_bytes_succeeded;
    uint64_t checkpoint_data_bytes_succeeded;
    uint64_t device_write_calls;
    uint64_t device_write_bytes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_bypasses;
};

class PerformanceScope
{
public:
    PerformanceScope(Performance *performance, PerformanceMetric Performance::*metric)
        : performance_(performance), metric_(performance ? &(performance->*metric) : nullptr), start_(0)
    {
        if (metric_) ++metric_->calls;
        if (performance_ && performance_->now) start_ = performance_->now(performance_->context);
    }

    ~PerformanceScope()
    {
        if (performance_ && performance_->now)
        {
            uint64_t end = performance_->now(performance_->context);
            if (end >= start_) metric_->nanoseconds += end - start_;
        }
    }

    PerformanceScope(const PerformanceScope &) = delete;
    PerformanceScope &operator=(const PerformanceScope &) = delete;

private:
    Performance *performance_;
    PerformanceMetric *metric_;
    uint64_t start_;
};

} // namespace xjext4
