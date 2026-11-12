// SPDX-License-Identifier: Apache-2.0
// Original bounded target/job admission. No user pointers or external runtime.

#include <installer/jobs.h>
#include <efi/installer_handoff.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <installer_protocol.h>
#include <cpu/lock.h>
#include <errno.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <user/permissions.h>

namespace installer
{
namespace
{
struct Owner
{
    uintptr_t cookie;
    uint64_t tid, pid;
};

enum class TargetState { Free, Preparing, Selected, Borrowed, Cleaning, Quarantined };
enum class InternalState { Free, Preparing, Published, Cleaning };

struct Target
{
    TargetState state;
    JobHandle handle;
    Owner owner;
    uint64_t epoch;
    DevfsBlockReference reference;
    BlockAccessToken probe;
};

struct Job
{
    InternalState internal;
    Owner owner, worker;
    JobSessionView session;
    DevfsBlockReference target;
    struct vfs_node *source_pin;
    JobRequest request;
    JobSnapshot snapshot;
};

spin_t registry_lock = SPIN_INIT;
Target targets[target_capacity];
Job jobs[job_capacity];
// Keep the opaque namespace disjoint from every legacy 32-bit disk slot.
uint64_t next_handle = 1ULL << 32;
JobHandle admitted_job;
bool service_quarantined;

class RegistryLock
{
public:
    RegistryLock() { spin_lock(&registry_lock); }
    ~RegistryLock() { spin_unlock(&registry_lock); }
};

bool same_owner(const Owner &a, const Owner &b)
{
    return a.cookie == b.cookie && a.tid == b.tid && a.pid == b.pid;
}

int caller(Owner *output, bool admin)
{
    *output = {};
    tcb_t task = get_current_task();
    if (!task || !task->parent_group || task->status == DEATH || task->status == OUT) return -ESRCH;
    // This runs in the original caller before a worker can be created. No
    // owner TCB/PCB pointer survives this call; only comparison cookies do.
    if (admin && !permissions_have(user_type_permission_mask(XUT_Admin))) return -EACCES;
    *output = {reinterpret_cast<uintptr_t>(task), task->tid, task->parent_group->pid};
    return 0;
}

bool same_extent(const BlockExtent &a, const BlockExtent &b)
{
    return a.device == b.device && a.physical_device == b.physical_device && a.generation == b.generation &&
           a.physical_generation == b.physical_generation && a.start == b.start && a.bytes == b.bytes;
}

bool same_session(const JobSessionView &a, const JobSessionView &b)
{
    return a.ready && b.ready && a.epoch == b.epoch && a.components == b.components &&
           a.profile == b.profile && a.language == b.language && a.source == b.source &&
           same_extent(a.source_extent, b.source_extent) && a.package == b.package &&
           a.package_bytes == b.package_bytes;
}

int current_session(JobSessionView *output)
{
    *output = {};
    if (!session_job_view(output) || !output->ready || !output->epoch) return -ENODEV;
    if (output->profile != XJ_INSTALLER_PROFILE_EXT4_PAK2 ||
        output->components != XJ380_INSTALLER_COMPONENT_ALL) return -EOPNOTSUPP;
    if (!output->source || !output->package || !output->package_bytes) return -EINVAL;
    return session_validate_job_view(*output);
}

int validate_target(const DevfsBlockReference &target, const JobSessionView &session)
{
    if (!target.node || target.quarantined || !target.bytes || !target.writable || target.sector_size != 512 ||
        target.bytes != target.extent.bytes || target.extent.start ||
        target.extent.device != target.extent.physical_device ||
        target.extent.generation != target.extent.physical_generation) return -EOPNOTSUPP;
    if (target.extent.physical_device == session.source_extent.physical_device) return -EBUSY;
    BlockExtent current = {};
    int error = block_access_resolve_node(target.node, &current);
    if (error) return error;
    if (!same_extent(current, target.extent)) return -ESTALE;
    return block_access_require_flush(current);
}

JobHandle issue_handle()
{
    if (!next_handle || next_handle == __UINT64_MAX__) return 0;
    return next_handle++;
}

Target *find_target(JobHandle handle)
{
    for (auto &target : targets)
        if (target.state != TargetState::Free && target.handle == handle) return &target;
    return nullptr;
}

Job *find_job(JobHandle handle)
{
    for (auto &job : jobs)
        if (job.internal != InternalState::Free && job.snapshot.handle == handle) return &job;
    return nullptr;
}

int close_target(Target *target)
{
    // The slot remains Cleaning and cannot be reused while VFS runs unlocked.
    int error = target->reference.quarantined ? -EIO : 0;
    if (!error && target->probe)
    {
        error = block_access_release(target->probe);
        if (!error) target->probe = 0;
    }
    if (!error && target->reference.node) error = vfs_close(target->reference.node);
    RegistryLock lock;
    if (error) target->state = TargetState::Quarantined;
    else *target = {};
    return error ? -EIO : 0;
}

int pin_source(Job *job)
{
    int error = 0;
    auto *source = job->session.source;
    vfs_child_lock();
    if (vfs_node_retiring_locked(source)) error = -EBUSY;
    else if (source->refcount == uint32_t(-1)) error = -EOVERFLOW;
    else { ++source->refcount; job->source_pin = source; }
    vfs_child_unlock();
    return error;
}

int clean_job(Job *job, bool release_slot)
{
    int error = 0;
    if (job->target.quarantined) error = -EIO;
    if (!error && job->source_pin)
    {
        error = vfs_close(job->source_pin);
        if (!error) job->source_pin = nullptr;
    }
    if (!error && job->target.node)
    {
        error = vfs_close(job->target.node);
        if (!error) job->target = {};
    }
    RegistryLock lock;
    if (error)
    {
        job->internal = InternalState::Published;
        job->snapshot.state = JobState::Quarantined;
        job->snapshot.error = -EIO;
        job->snapshot.outcome_unknown = true;
        service_quarantined = true;
    }
    else
    {
        if (admitted_job == job->snapshot.handle) admitted_job = 0;
        if (release_slot) *job = {};
        else job->internal = InternalState::Published;
    }
    return error ? -EIO : 0;
}
} // namespace

int jobs_select_target(uint32_t device, uint64_t generation, JobHandle *output)
{
    if (!output) return -EINVAL;
    *output = 0;
    if (!generation) return -EINVAL;
    Owner owner = {};
    int error = caller(&owner, true);
    if (error) return error;
    JobSessionView session = {};
    error = current_session(&session);
    if (error) return error;
    Target *target = nullptr;
    {
        RegistryLock lock;
        if (service_quarantined) return -EIO;
        for (auto &candidate : targets)
            if (candidate.state == TargetState::Free) { target = &candidate; break; }
        if (!target) return -ENOSPC;
        JobHandle handle = issue_handle();
        if (!handle) return -EOVERFLOW;
        *target = {};
        target->state = TargetState::Preparing;
        target->handle = handle;
        target->owner = owner;
        target->epoch = session.epoch;
    }
    // Static slot storage owns every pin, including a failed/quarantined retain.
    error = devfs_retain_block(device, generation, &target->reference);
    if (!error) error = validate_target(target->reference, session);
    if (!error) error = block_access_claim(target->reference.extent, true, &target->probe);
    if (!error)
    {
        error = block_access_release(target->probe);
        if (!error) target->probe = 0;
        else target->reference.quarantined = true;
    }
    Owner now = {};
    if (!error) error = caller(&now, true);
    if (!error && !same_owner(now, owner)) error = -ESTALE;
    JobSessionView current = {};
    if (!error) error = current_session(&current);
    if (!error && !same_session(session, current)) error = -ESTALE;
    {
        RegistryLock lock;
        if (!error && !service_quarantined)
        {
            target->state = TargetState::Selected;
            *output = target->handle;
            return 0;
        }
        if (!error) error = -EIO;
        target->state = TargetState::Cleaning;
    }
    int close = close_target(target);
    return close ? close : error;
}

int jobs_release_target(JobHandle handle)
{
    Owner owner = {};
    int error = caller(&owner, false);
    if (error) return error;
    Target *target;
    {
        RegistryLock lock;
        target = find_target(handle);
        if (!target) return -ENOENT;
        if (!same_owner(target->owner, owner)) return -EACCES;
        if (target->state == TargetState::Quarantined) return -EIO;
        if (target->state != TargetState::Selected) return -EBUSY;
        target->state = TargetState::Cleaning;
    }
    return close_target(target);
}

int jobs_queue(JobHandle target_handle, const JobRequest *request, JobHandle *output)
{
    if (!output) return -EINVAL;
    *output = 0;
    if (!request) return -EINVAL;
    JobRequest snapshot = *request;
    Owner owner = {};
    int error = caller(&owner, true);
    if (error) return error;
    JobSessionView session = {};
    error = current_session(&session);
    if (error) return error;
    if (snapshot.mode != XJ380_INSTALLER_MODE_FRESH || snapshot.language != session.language ||
        snapshot.components != session.components) return -EOPNOTSUPP;
    Target *target;
    Job *job = nullptr;
    {
        RegistryLock lock;
        if (service_quarantined) return -EIO;
        if (admitted_job) return -EBUSY;
        target = find_target(target_handle);
        if (!target) return -ENOENT;
        if (!same_owner(target->owner, owner)) return -EACCES;
        if (target->state != TargetState::Selected) return -EBUSY;
        if (target->epoch != session.epoch) return -ESTALE;
        for (auto &candidate : jobs)
            if (candidate.internal == InternalState::Free) { job = &candidate; break; }
        if (!job) return -ENOSPC;
        JobHandle handle = issue_handle();
        if (!handle) return -EOVERFLOW;
        *job = {};
        job->internal = InternalState::Preparing;
        job->owner = owner;
        job->session = session;
        job->request = snapshot;
        job->snapshot.handle = handle;
        job->snapshot.session_epoch = session.epoch;
        job->snapshot.state = JobState::Queued;
        target->state = TargetState::Borrowed;
        admitted_job = handle;
    }
    error = validate_target(target->reference, session);
    if (!error) error = pin_source(job);
    Owner now = {};
    if (!error) error = caller(&now, true);
    if (!error && !same_owner(now, owner)) error = -ESTALE;
    JobSessionView current = {};
    if (!error) error = current_session(&current);
    if (!error && !same_session(session, current)) error = -ESTALE;
    {
        RegistryLock lock;
        if (!error && !service_quarantined)
        {
            job->target = target->reference;
            *target = {};
            job->internal = InternalState::Published;
            *output = job->snapshot.handle;
            return 0;
        }
        if (!error) error = -EIO;
        target->state = TargetState::Selected;
        job->internal = InternalState::Cleaning;
    }
    int close = clean_job(job, true);
    return close ? close : error;
}

int jobs_query(JobHandle handle, JobSnapshot *output)
{
    if (!output) return -EINVAL;
    *output = {};
    Owner owner = {};
    int error = caller(&owner, true);
    if (error) return error;
    RegistryLock lock;
    Job *job = find_job(handle);
    if (!job) return -ENOENT;
    if (!same_owner(job->owner, owner)) return -EACCES;
    if (job->internal != InternalState::Published) return -EAGAIN;
    *output = job->snapshot;
    return 0;
}

int jobs_owned_epoch(JobHandle handle, uint64_t *output)
{
    if (!output) return -EINVAL;
    *output = 0;
    Owner owner = {};
    int error = caller(&owner, false);
    if (error) return error;
    RegistryLock lock;
    Job *job = find_job(handle);
    if (!job) return -ENOENT;
    if (!same_owner(job->owner, owner)) return -EACCES;
    if (job->internal != InternalState::Published) return -EAGAIN;
    *output = job->snapshot.session_epoch;
    return 0;
}

int jobs_cancel_queued(JobHandle handle)
{
    Owner owner = {};
    int error = caller(&owner, false);
    if (error) return error;
    Job *job;
    {
        RegistryLock lock;
        job = find_job(handle);
        if (!job) return -ENOENT;
        if (!same_owner(job->owner, owner)) return -EACCES;
        if (job->internal != InternalState::Published || job->snapshot.state != JobState::Queued) return -EBUSY;
        job->snapshot.state = JobState::Cancelled;
        job->snapshot.error = -EINTR;
        job->internal = InternalState::Cleaning;
    }
    return clean_job(job, false);
}

int jobs_release(JobHandle handle)
{
    Owner owner = {};
    int error = caller(&owner, false);
    if (error) return error;
    Job *job;
    {
        RegistryLock lock;
        job = find_job(handle);
        if (!job) return -ENOENT;
        if (!same_owner(job->owner, owner)) return -EACCES;
        if (job->internal != InternalState::Published) return -EBUSY;
        if (job->snapshot.state == JobState::Quarantined) return -EIO;
        if (job->snapshot.state == JobState::Queued || job->snapshot.state == JobState::Running) return -EBUSY;
        job->internal = InternalState::Cleaning;
    }
    return clean_job(job, true);
}

int jobs_run(JobHandle handle)
{
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_KERNEL_LEVEL || task->parent_group != kernel_group ||
        task->status == DEATH || task->status == OUT) return -EPERM;
    Job *job;
    {
        RegistryLock lock;
        job = find_job(handle);
        if (!job) return -ENOENT;
        if (service_quarantined) return -EIO;
        if (job->internal != InternalState::Published || job->snapshot.state != JobState::Queued) return -EBUSY;
        // Only a previously admitted queue entry can become Running. No call
        // to permissions_have here can replace the original caller's grant.
        job->worker = {reinterpret_cast<uintptr_t>(task), task->tid, task->parent_group->pid};
        job->snapshot.state = JobState::Running;
    }
    JobSessionView current = {};
    int error = current_session(&current);
    if (!error && !same_session(job->session, current)) error = -ESTALE;
    if (!error) error = validate_target(job->target, current);
    xjinstaller::FactoryStatus status = {};
    if (!error)
        error = installer_build_ext4_disk(job->target.node, job->source_pin, &job->request.factory,
                                          job->session.package, job->session.package_bytes, &status);
    if (!error && (!status.complete || !status.activation_durable || status.io_failed || status.quarantined))
    {
        error = -EIO;
        status.quarantined = true;
    }
    if (error && (status.io_started || status.io_failed || status.activation_attempted)) status.quarantined = true;
    status.error = error;
    {
        RegistryLock lock;
        // Quiescent abandonment never races live execution, but a retained
        // tombstone must not be overwritten if a future dispatcher violates it.
        if (job->snapshot.state == JobState::Quarantined) return -EIO;
        job->snapshot.factory = status;
        job->snapshot.error = error;
        if (status.quarantined)
        {
            job->snapshot.state = JobState::Quarantined;
            job->snapshot.outcome_unknown = true;
            service_quarantined = true;
        }
        else
        {
            job->snapshot.state = error ? JobState::Failed : JobState::Succeeded;
            if (admitted_job == handle) admitted_job = 0;
        }
    }
    return error;
}

void jobs_abandon_worker(const void *task, uint64_t tid)
{
    if (!task) return;
    uintptr_t cookie = reinterpret_cast<uintptr_t>(task);
    RegistryLock lock;
    for (auto &job : jobs)
    {
        if (job.internal != InternalState::Published || job.snapshot.state != JobState::Running ||
            job.worker.cookie != cookie || job.worker.tid != tid) continue;
        job.snapshot.state = JobState::Quarantined;
        job.snapshot.error = -EINTR;
        job.snapshot.outcome_unknown = true;
        service_quarantined = true;
    }
}

void jobs_reap_abandoned()
{
    RegistryLock lock;
    for (auto &job : jobs)
    {
        if (job.internal != InternalState::Published || job.snapshot.state != JobState::Running) continue;
        // A Running record is withdrawn under this same lock before the TCB
        // may be freed. Quarantined/completed cookies must never be decoded.
        tcb_t worker = reinterpret_cast<tcb_t>(job.worker.cookie);
        bool abandoned = !worker;
        if (worker)
        {
            uint64_t tid = __atomic_load_n(&worker->tid, __ATOMIC_ACQUIRE);
            TaskStatus status = __atomic_load_n(&worker->status, __ATOMIC_ACQUIRE);
            abandoned = tid != job.worker.tid ||
                        ((status == DEATH || status == OUT) && !scheduler_task_referenced(worker));
        }
        if (!abandoned) continue;
        job.snapshot.state = JobState::Quarantined;
        job.snapshot.error = -EINTR;
        job.snapshot.outcome_unknown = true;
        service_quarantined = true;
    }
}
} // namespace installer
