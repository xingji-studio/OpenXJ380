// SPDX-License-Identifier: Apache-2.0
// Original bounded block-device lifetime and interval exclusion registry.
#include <block_access.h>
#include <errno.h>
#include <task/pcb.h>
#include <task/scheduler.h>

namespace {

constexpr unsigned kDevices = 256, kLeases = 64, kTickets = 128;
struct Registration { BlockExtent extent; bool valid, published, flush_supported; } devices[kDevices];
struct Access
{
    BlockExtent extent;
    BlockAccessToken token;
    const void *owner;
    uint64_t tid;
    bool exclusive, poisoned, scan;
};
Access leases[kLeases], tickets[kTickets];
Access management;
uint32_t management_depth;
uint64_t next_identity = 1;
unsigned char access_lock;
bool io_abandoned;

static uint64_t access_irq_save()
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void access_irq_restore(uint64_t flags)
{
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

class Lock
{
    uint64_t flags;
public:
    Lock() : flags(access_irq_save())
    {
        while (__atomic_test_and_set(&access_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    }
    ~Lock()
    {
        __atomic_clear(&access_lock, __ATOMIC_RELEASE);
        access_irq_restore(flags);
    }
};

static uint64_t identity()
{
    if (next_identity == uint64_t(-1)) return 0;
    return next_identity++;
}

static bool same_disk(const BlockExtent &a, const BlockExtent &b)
{
    return a.physical_device == b.physical_device && a.physical_generation == b.physical_generation;
}

static bool overlaps(const BlockExtent &a, const BlockExtent &b)
{
    return same_disk(a, b) && a.bytes && b.bytes && a.start < b.start + b.bytes && b.start < a.start + a.bytes;
}

static bool contains(const BlockExtent &a, const BlockExtent &b)
{
    return same_disk(a, b) && b.start >= a.start && b.start - a.start <= a.bytes &&
           b.bytes <= a.bytes - (b.start - a.start);
}

static bool owner(const Access &entry, tcb_t task)
{
    return entry.owner == task && entry.tid == (task ? task->tid : 0);
}

static bool owner_stopped(const Access &entry)
{
    // TCB destruction first takes this same access lock in abandon_owner.
    // Poisoned tombstones may retain freed pointer values: never dereference
    // them, including after an allocator reuses that address for another TID.
    if (!entry.token || entry.poisoned || !entry.owner) return false;
    tcb_t task = (tcb_t)entry.owner;
    TaskStatus status = __atomic_load_n(&task->status, __ATOMIC_ACQUIRE);
    return (status == DEATH || status == OUT) && !scheduler_task_referenced(task);
}

static bool resolve(uint32_t device, uint64_t generation, BlockExtent &extent)
{
    if (device >= kDevices || !devices[device].valid || !devices[device].published) return false;
    const BlockExtent &entry = devices[device].extent;
    if (generation && entry.generation != generation) return false;
    if (!devices[entry.physical_device].valid || !devices[entry.physical_device].published ||
        devices[entry.physical_device].extent.generation != entry.physical_generation) return false;
    extent = entry;
    return true;
}

static bool exact(const BlockExtent &a, const BlockExtent &b)
{
    return a.device == b.device && a.generation == b.generation && same_disk(a, b) &&
           a.start == b.start && a.bytes == b.bytes;
}

static int available(const BlockExtent &extent, bool registration = false)
{
    tcb_t task = get_current_task();
    for (const Access &entry : leases)
        if (entry.token && overlaps(entry.extent, extent))
        {
            if (entry.poisoned) return -EIO;
            if (!registration || !entry.scan || !owner(entry, task) || !contains(entry.extent, extent))
                return -EBUSY;
        }
    for (const Access &entry : tickets)
        if (entry.token && overlaps(entry.extent, extent)) return entry.poisoned ? -EIO : -EBUSY;
    return 0;
}

static int claim(const BlockExtent &extent, bool exclusive, bool scan, BlockAccessToken *token)
{
    if (io_abandoned) return -EIO;
    BlockExtent current;
    if (!resolve(extent.device, extent.generation, current) || !exact(extent, current)) return -ENODEV;
    tcb_t task = get_current_task();
    if (exclusive && !scan && !task) return -EPERM;
    Access *slot = nullptr;
    for (Access &entry : leases)
    {
        if (!entry.token && !slot) slot = &entry;
        if (!entry.token || !overlaps(entry.extent, extent)) continue;
        if (entry.poisoned) return -EIO;
        // A second filesystem cache must never mount an overlapping alias.
        return -EBUSY;
    }
    for (const Access &entry : tickets)
        if (entry.token && overlaps(entry.extent, extent))
            return entry.poisoned ? -EIO : -EBUSY;
    if (!slot) return -ENOSPC;
    uint64_t value = identity();
    if (!value) return -EOVERFLOW;
    *slot = {extent, value, exclusive ? task : nullptr, exclusive && task ? task->tid : 0,
             exclusive, false, scan};
    *token = value;
    return 0;
}

} // namespace

int block_access_register_whole(uint32_t device, uint64_t bytes, bool flush_supported)
{
    if (device >= kDevices || !bytes) return -EINVAL;
    Lock lock;
    if (devices[device].valid)
    {
        int error = available(devices[device].extent);
        if (error) return error;
        return -EEXIST;
    }
    uint64_t generation = identity();
    if (!generation) return -EOVERFLOW;
    devices[device] = {{device, device, generation, generation, 0, bytes}, true, false, flush_supported};
    return 0;
}

int block_access_register_slice(uint32_t device, uint32_t parent, uint64_t offset, uint64_t bytes)
{
    if (device >= kDevices || !bytes) return -EINVAL;
    Lock lock;
    if (devices[device].valid) return -EEXIST;
    BlockExtent base;
    if (!resolve(parent, 0, base)) return -ENODEV;
    if (offset > base.bytes || bytes > base.bytes - offset) return -EINVAL;
    BlockExtent extent = {device, base.physical_device, 0, base.physical_generation, base.start + offset, bytes};
    int error = available(extent, true);
    if (error) return error;
    extent.generation = identity();
    if (!extent.generation) return -EOVERFLOW;
    devices[device] = {extent, true, false, false};
    return 0;
}

int block_access_publish_device(uint32_t device, uint64_t generation)
{
    Lock lock;
    if (device >= kDevices || !devices[device].valid || devices[device].extent.generation != generation)
        return -ENODEV;
    devices[device].published = true;
    return 0;
}

int block_access_unregister(uint32_t device)
{
    Lock lock;
    if (device >= kDevices || !devices[device].valid) return -ENODEV;
    BlockExtent extent = devices[device].extent;
    int error = available(extent);
    if (error) return error;
    devices[device].valid = devices[device].published = false;
    if (extent.device == extent.physical_device)
        for (Registration &entry : devices)
            if (entry.valid && same_disk(entry.extent, extent)) entry.valid = entry.published = false;
    return 0;
}

uint64_t block_access_generation(uint32_t device)
{
    Lock lock;
    return device < kDevices && devices[device].valid ?
           devices[device].extent.generation : 0;
}

int block_access_resolve(uint32_t device, uint64_t generation, BlockExtent *extent)
{
    if (!extent) return -EINVAL;
    Lock lock;
    return resolve(device, generation, *extent) ? 0 : -ENODEV;
}

int block_access_require_flush(const BlockExtent &extent)
{
    Lock lock;
    if (io_abandoned) return -EIO;
    BlockExtent current;
    if (!resolve(extent.device, extent.generation, current) || !exact(current, extent)) return -ENODEV;
    for (const Access &entry : leases)
        if (entry.token && entry.poisoned && overlaps(entry.extent, extent)) return -EIO;
    for (const Access &entry : tickets)
        if (entry.token && entry.poisoned && overlaps(entry.extent, extent)) return -EIO;
    return devices[extent.physical_device].flush_supported ? 0 : -EOPNOTSUPP;
}

int block_access_claim(const BlockExtent &extent, bool exclusive, BlockAccessToken *token)
{
    if (!token) return -EINVAL;
    *token = 0;
    Lock lock;
    return claim(extent, exclusive, false, token);
}

int block_access_release(BlockAccessToken token)
{
    Lock lock;
    for (Access &entry : leases)
        if (entry.token == token && token)
        {
            if (entry.poisoned) return -EIO;
            if (entry.exclusive && !owner(entry, get_current_task())) return -EPERM;
            entry = {};
            return 0;
        }
    return -EINVAL;
}

void block_access_poison(BlockAccessToken token)
{
    Lock lock;
    for (Access &entry : leases) if (entry.token == token && token) entry.poisoned = true;
    for (Access &entry : tickets) if (entry.token == token && token) entry.poisoned = true;
}

int block_access_validate_owned_exclusive(BlockAccessToken token, const BlockExtent &whole,
                                          uint64_t offset, uint64_t bytes)
{
    // Byte arithmetic is checked before addition; the caller cannot turn a
    // registered slice or a forged extent into whole-device authority.
    if (!token || !bytes || ((offset | bytes | whole.bytes) & 511) ||
        offset > whole.bytes || bytes > whole.bytes - offset) return -EINVAL;
    Lock lock;
    if (io_abandoned) return -EIO;
    BlockExtent current;
    if (!resolve(whole.device, whole.generation, current) || !exact(whole, current)) return -ENODEV;
    if (current.device != current.physical_device || current.start ||
        current.generation != current.physical_generation) return -EINVAL;
    const Access *parent = nullptr;
    for (const Access &entry : leases)
    {
        if (entry.token && entry.poisoned && overlaps(entry.extent, current)) return -EIO;
        if (entry.token == token) parent = &entry;
    }
    for (const Access &entry : tickets)
        if (entry.token && entry.poisoned && overlaps(entry.extent, current)) return -EIO;
    if (!parent) return -EINVAL;
    if (!parent->exclusive || parent->scan) return -EPERM;
    if (!exact(parent->extent, current)) return -EINVAL;
    tcb_t task = get_current_task();
    if (!task || !owner(*parent, task)) return -EPERM;
    TaskStatus status = __atomic_load_n(&task->status, __ATOMIC_ACQUIRE);
    if (status == DEATH || status == OUT) return -EIO;
    return 0;
}

int block_access_validate_retained_exclusive(BlockAccessToken token, const BlockExtent &whole)
{
    if (!token || !whole.bytes || whole.device != whole.physical_device || whole.start ||
        whole.generation != whole.physical_generation) return -EINVAL;
    Lock lock;
    if (io_abandoned) return -EIO;
    BlockExtent current;
    if (!resolve(whole.device, whole.generation, current) || !exact(whole, current)) return -ENODEV;
    const Access *retained = nullptr;
    for (const Access &entry : leases)
    {
        if (entry.token && entry.poisoned && overlaps(entry.extent, current)) return -EIO;
        if (entry.token == token) retained = &entry;
    }
    for (const Access &entry : tickets)
        if (entry.token && entry.poisoned && overlaps(entry.extent, current)) return -EIO;
    if (!retained) return -EINVAL;
    if (!retained->exclusive || retained->scan || !retained->owner) return -EPERM;
    if (!exact(retained->extent, current)) return -EINVAL;
    // The registry lock also guards the mandatory abandon-before-TCB-free
    // hook. Poisoned entries return above without dereferencing stale owners.
    tcb_t task = (tcb_t)retained->owner;
    TaskStatus status = __atomic_load_n(&task->status, __ATOMIC_ACQUIRE);
    if (task->tid != retained->tid || status == DEATH || status == OUT) return -EIO;
    return 0;
}

int block_access_io_begin(uint32_t device, uint64_t generation, uint64_t offset, uint64_t bytes,
                          BlockAccessToken *token)
{
    if (!token) return -EINVAL;
    *token = 0;
    Lock lock;
    BlockExtent extent;
    if (io_abandoned) return -EIO;
    if (!resolve(device, generation, extent)) return -ENODEV;
    if (offset > extent.bytes || bytes > extent.bytes - offset) return -EINVAL;
    extent.start += offset;
    extent.bytes = bytes;
    tcb_t task = get_current_task();
    for (const Access &entry : leases)
        if (entry.token && overlaps(entry.extent, extent))
        {
            if (entry.poisoned) return -EIO;
            if (entry.exclusive && (!owner(entry, task) || !contains(entry.extent, extent))) return -EBUSY;
        }
    Access *slot = nullptr;
    for (Access &entry : tickets)
    {
        if (!entry.token && !slot) slot = &entry;
        if (entry.token && entry.poisoned && overlaps(entry.extent, extent)) return -EIO;
    }
    if (!slot) return -ENOSPC;
    uint64_t value = identity();
    if (!value) return -EOVERFLOW;
    *slot = {extent, value, task, task ? task->tid : 0, false, false, false};
    *token = value;
    return 0;
}

int block_access_flush_begin(uint32_t device, uint64_t generation, BlockAccessToken *token)
{
    BlockExtent extent;
    int error = block_access_resolve(device, generation, &extent);
    return error ? error : block_access_io_begin(device, extent.generation, 0, extent.bytes, token);
}

void block_access_io_end(BlockAccessToken token)
{
    Lock lock;
    for (Access &entry : tickets)
        if (entry.token == token && token && !entry.poisoned) { entry = {}; return; }
}

void block_access_abandon_owner(const void *task, uint64_t tid)
{
    if (!task) return;
    Lock lock;
    for (Access &entry : leases)
        if (entry.token && entry.exclusive && entry.owner == task && entry.tid == tid) entry.poisoned = true;
    for (Access &entry : tickets)
        if (entry.token && entry.owner == task && entry.tid == tid)
        {
            entry.poisoned = true;
            // A driver-wide sleeping mutex or DMA request may still belong to
            // this task. Stop new requests globally; this does not cancel or
            // reclaim requests already waiting or submitted to the backend.
            io_abandoned = true;
        }
    if (management.token && management.owner == task && management.tid == tid) management.poisoned = true;
}

void block_access_reap_abandoned()
{
    Lock lock;
    for (Access &entry : leases)
        if (entry.exclusive && owner_stopped(entry)) entry.poisoned = true;
    for (Access &entry : tickets)
        if (owner_stopped(entry))
        {
            entry.poisoned = true;
            io_abandoned = true;
        }
    if (owner_stopped(management)) management.poisoned = true;
}

int block_access_scan_begin(uint32_t device, BlockAccessToken *token)
{
    if (!token) return -EINVAL;
    *token = 0;
    Lock lock;
    BlockExtent extent;
    if (!resolve(device, 0, extent)) return -ENODEV;
    if (extent.device != extent.physical_device) return -EINVAL;
    return claim(extent, true, true, token);
}

int block_access_management_begin(BlockAccessToken *token)
{
    if (!token) return -EINVAL;
    *token = 0;
    Lock lock;
    tcb_t task = get_current_task();
    if (management.poisoned) return -EIO;
    if (management.token)
    {
        if (!owner(management, task)) return -EBUSY;
        if (management_depth == uint32_t(-1)) return -EOVERFLOW;
        ++management_depth;
    }
    else
    {
        uint64_t value = identity();
        if (!value) return -EOVERFLOW;
        management = {{}, value, task, task ? task->tid : 0, true, false, false};
        management_depth = 1;
    }
    *token = management.token;
    return 0;
}

void block_access_management_end(BlockAccessToken token)
{
    Lock lock;
    if (token && management.token == token && !management.poisoned && owner(management, get_current_task()) &&
        management_depth && !--management_depth)
        management = {};
}
