// SPDX-License-Identifier: Apache-2.0
// Original boot-source reference/physical exclusion lifetime. No source I/O.

#include <installer/source_lifetime.h>
#include <block_source.h>
#include <errno.h>
#include <task/pcb.h>

namespace installer
{
namespace
{
enum SourceState : unsigned
{
    empty,
    retaining,
    retained,
    published,
    quarantined
};

bool same_extent(const BlockExtent &a, const BlockExtent &b)
{
    return a.device == b.device && a.physical_device == b.physical_device && a.generation == b.generation &&
           a.physical_generation == b.physical_generation && a.start == b.start && a.bytes == b.bytes;
}

bool boot_owner(const SourceLifetime &source)
{
    tcb_t task = get_current_task();
    return task && source.owner == task && source.owner_tid == task->tid;
}

int cleanup_unpublished(SourceLifetime &source, int failure)
{
    // Release the physical exclusion only after the real node reference closed
    // successfully. On any uncertain cleanup preserve all remaining resources.
    if (source.reference.node)
    {
        int result = vfs_close(source.reference.node);
        if (result)
        {
            quarantine_source(&source);
            return result < 0 ? result : -EIO;
        }
        source.reference = {};
    }
    if (source.lease)
    {
        int result = block_access_release(source.lease);
        if (result)
        {
            quarantine_source(&source);
            return result;
        }
        source.lease = 0;
    }
    source.identity = {};
    source.owner = nullptr;
    source.owner_tid = 0;
    __atomic_store_n(&source.state, empty, __ATOMIC_RELEASE);
    return failure;
}
} // namespace

int retain_source(const XJ_INSTALLER_SOURCE &identity, SourceLifetime *source)
{
    if (!source) return -EINVAL;
    tcb_t task = get_current_task();
    if (!task) return -EPERM;
    TaskStatus status = __atomic_load_n(&task->status, __ATOMIC_ACQUIRE);
    if (status == DEATH || status == OUT) return -EPERM;
    unsigned expected = empty;
    if (!__atomic_compare_exchange_n(&source->state, &expected, retaining, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -EBUSY;
    source->identity = identity;
    source->owner = task;
    source->owner_tid = task->tid;
    BlockExtent expected_extent = {};
    int result = block_source_match(source->identity, &expected_extent);
    if (result) return cleanup_unpublished(*source, result);
    result = block_access_claim(expected_extent, true, &source->lease);
    if (result) return cleanup_unpublished(*source, result);
    result = devfs_retain_block(expected_extent.device, expected_extent.generation, &source->reference);
    if (result)
    {
        if (source->reference.quarantined)
        {
            quarantine_source(source);
            return result;
        }
        return cleanup_unpublished(*source, result);
    }
    if (!same_extent(expected_extent, source->reference.extent))
        return cleanup_unpublished(*source, -ESTALE);
    // Re-match the actual hardware and captured handle after both lifetimes
    // exist. A duplicate registration or stale identity cannot be published.
    BlockExtent matched = {}, from_node = {};
    result = block_source_match(source->identity, &matched);
    if (!result) result = block_access_resolve_node(source->reference.node, &from_node);
    if (!result && (!same_extent(matched, expected_extent) || !same_extent(from_node, expected_extent) ||
                    source->reference.bytes != identity.physical_bytes ||
                    source->reference.sector_size != identity.physical_block_size)) result = -ESTALE;
    if (!result) result = block_access_validate_retained_exclusive(source->lease, expected_extent);
    if (result) return cleanup_unpublished(*source, result);
    __atomic_store_n(&source->state, retained, __ATOMIC_RELEASE);
    return 0;
}

int validate_source(const SourceLifetime &source)
{
    unsigned state = __atomic_load_n(&source.state, __ATOMIC_ACQUIRE);
    if (state == quarantined) return -EIO;
    if ((state != retained && state != published) || source.reference.quarantined ||
        !source.reference.node || !source.lease) return -ENODEV;
    int result = block_access_validate_retained_exclusive(source.lease, source.reference.extent);
    if (result) return result;
    BlockExtent matched = {}, from_node = {};
    result = block_source_match(source.identity, &matched);
    if (!result) result = block_access_resolve_node(source.reference.node, &from_node);
    if (!result && (!same_extent(matched, source.reference.extent) ||
                    !same_extent(from_node, source.reference.extent) ||
                    source.reference.bytes != source.identity.physical_bytes ||
                    source.reference.sector_size != source.identity.physical_block_size)) result = -ESTALE;
    return result;
}

int publish_source(SourceLifetime *source)
{
    if (!source) return -EINVAL;
    if (!boot_owner(*source)) return -EPERM;
    int result = validate_source(*source);
    if (result) return result;
    unsigned expected = retained;
    if (!__atomic_compare_exchange_n(&source->state, &expected, published, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -EBUSY;
    return 0;
}

int release_source_boot_only(SourceLifetime *source)
{
    if (!source) return -EINVAL;
    if (!boot_owner(*source)) return -EPERM;
    unsigned expected = retained;
    if (!__atomic_compare_exchange_n(&source->state, &expected, retaining, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -EPERM;
    return cleanup_unpublished(*source, 0);
}

void quarantine_source(SourceLifetime *source)
{
    if (!source) return;
    if (source->lease) block_access_poison(source->lease);
    __atomic_store_n(&source->state, quarantined, __ATOMIC_RELEASE);
}

vfs_node_t source_node(const SourceLifetime &source)
{
    return validate_source(source) ? nullptr : source.reference.node;
}
} // namespace installer
