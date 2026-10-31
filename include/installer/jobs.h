#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <block_access.h>
#include <installer/factory.h>

namespace installer
{
using JobHandle = uint64_t;
constexpr unsigned target_capacity = 32;
constexpr unsigned job_capacity = 8;

// Kernel-only immutable session view. The session owns the source and package
// lifetime through all jobs, including abandoned/quarantined jobs, until reboot.
// Neither this structure nor a raw node pointer is a user ABI or write grant.
struct JobSessionView
{
    uint64_t epoch, components;
    uint32_t profile, language;
    struct vfs_node *source;
    BlockExtent source_extent;
    const void *package;
    __SIZE_TYPE__ package_bytes;
    bool ready;
};

// Implemented by the boot/session owner. Validation reauthenticates the retained
// source registration and lease without transferring its I/O ownership.
bool session_job_view(JobSessionView *);
int session_validate_job_view(const JobSessionView &);

struct JobRequest
{
    uint32_t mode, language;
    uint64_t components;
    // Trusted kernel-generated fresh-disk identities/geometry only. A syscall
    // must never copy arbitrary user GUID/LBA choices into this structure.
    xjinstaller::FactoryOptions factory;
};

enum class JobState : uint32_t
{
    Queued, Running, Succeeded, Failed, Cancelled, Quarantined
};

struct JobSnapshot
{
    JobHandle handle;
    uint64_t session_epoch;
    JobState state;
    int error;
    bool outcome_unknown;
    // Running snapshots intentionally contain no concurrently read factory
    // output. The synchronous factory publishes its final snapshot on return.
    xjinstaller::FactoryStatus factory;
};

// All pointers name kernel storage. Selection is a registry lookup, not authority:
// it checks the live issuing caller's Admin permission, then returns an opaque,
// monotonically issued, same-TCB/TID/PID handle for one retained devfs node.
// Its temporary zero-I/O exclusive probe is advisory; the actual worker must
// acquire a fresh factory lease. Raw legacy disk slots cannot queue a write.
// Unreleased handles from a vanished issuing thread conservatively retain their
// bounded slots/pins until reboot; they cannot be adopted through TCB reuse.
int jobs_select_target(uint32_t device, uint64_t generation, JobHandle *);
int jobs_release_target(JobHandle);
int jobs_queue(JobHandle target, const JobRequest *, JobHandle *job);
int jobs_query(JobHandle, JobSnapshot *);
// Cleanup-only correlation lookup for the same issuing owner after permission
// revocation. It returns no target, source or I/O authority.
int jobs_owned_epoch(JobHandle, uint64_t *);
int jobs_cancel_queued(JobHandle);
int jobs_release(JobHandle);

// Called only by a dedicated kernel-group worker after the original caller
// successfully queued the job. The worker's inherited Root identity never
// grants or repairs admission. Factory execution and its exclusive target
// lease remain in this same worker TCB; no lease transfer occurs.
int jobs_run(JobHandle);

// Quiescent reaper hook, before freeing/reusing a worker TCB. Only compares the
// integer address cookie and monotonic TID; never dereferences an old owner.
// Marks an interrupted job unknown/quarantined and retains every pin until reboot.
void jobs_abandon_worker(const void *task, uint64_t tid);

// Periodic task-context reaper for a terminal worker in a still-live kernel
// process. Running-record pointers remain readable under the registry lock
// because jobs_abandon_worker takes that same lock before TCB destruction.
// Scheduler current/selected/previous-stack references postpone abandonment.
void jobs_reap_abandoned();
} // namespace installer
