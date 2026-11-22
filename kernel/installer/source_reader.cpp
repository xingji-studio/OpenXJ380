// SPDX-License-Identifier: Apache-2.0
#include <fs/vfs/vfs.h>
#include <installer/source_reader.h>
#include <krlibc.h>
#include <task/pcb.h>

namespace installer {
int read_source_device(void *opaque, uint64_t physical, void *output, size_t bytes)
{
    if (!opaque || !output || !bytes) return -22;
    auto &reader = *static_cast<SourceReadContext *>(opaque);
    if (!reader.source) return -22;
    auto &source = *reader.source;
    tcb_t task   = get_current_task();
    if (!task || task->status == DEATH || task->status == OUT) return -1;
    // KernelMain still runs on the BSP idle TCB. Only that retained boot
    // owner may resolve carrier metadata before publication; runtime data
    // reads retain the existing kernel-worker-only admission.
    bool boot_owner = reader.preparing && task->task_level == TASK_IDLE_LEVEL && source.owner == task &&
                      source.owner_tid == task->tid;
    if (task->task_level != TASK_KERNEL_LEVEL && !boot_owner) return -1;
    int         error    = validate_source(source);
    const auto &identity = source.identity;
    uint64_t    block    = identity.physical_block_size;
    if (!error && (!source.read_only_stream || source.reference.writable || !block || block > sizeof(reader.bounce) ||
                   block & (block - 1)))
        error = -95;
    unsigned prior_privilege = task->ime_storage_depth;
    if (!error && !prior_privilege) task->ime_storage_depth = 1;
    size_t done = 0;
    while (!error && done < bytes)
    {
        if (physical > identity.physical_bytes || done > identity.physical_bytes - physical)
        {
            error = -22;
            break;
        }
        uint64_t position = physical + done;
        uint64_t aligned = position & ~(block - 1), within = position - aligned;
        size_t   available = bytes - done;
        size_t amount = available < sizeof(reader.bounce) - within ? available : sizeof(reader.bounce) - size_t(within);
        size_t request = (size_t(within) + amount + size_t(block) - 1) & ~(size_t(block) - 1);
        if (aligned > identity.physical_bytes || request > identity.physical_bytes - aligned ||
            vfs_read(source.reference.node, reader.bounce, aligned, request) != request)
        {
            error = -5;
            break;
        }
        // A physical disconnect can race the completed transfer. Recheck the
        // native registration/guard before publishing any bytes to the parser.
        error = validate_source(source);
        if (error) break;
        memcpy(static_cast<uint8_t *>(output) + done, reader.bounce + within, amount);
        done += amount;
    }
    task->ime_storage_depth = prior_privilege;
    return error;
}

int read_stream_source(void *opaque, uint64_t offset, void *output, size_t bytes)
{
    if (!opaque || !output || !bytes) return -22;
    auto &reader = *static_cast<SourceReadContext *>(opaque);
    if (!reader.source || !reader.index) return -22;
    const auto &index = *reader.index;
    if (offset > index.data_bytes || bytes > index.data_bytes - offset) return -22;
    bool expected = false;
    // The private filesystem workspace and bounce buffer share one admission.
    // No lock, wait or scheduler gate covers parsing or physical source I/O.
    if (!__atomic_compare_exchange_n(&reader.busy, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return -16;
    int error = validate_source(*reader.source);
    if (!error && index.version == 3)
    {
        error = reader.files.read ? reader.files.read(reader.files.context, offset, output, bytes) : -19;
    }
    else
    {
        size_t done = 0;
        while (!error && done < bytes)
        {
            uint64_t volume_offset = 0;
            size_t   available     = 0;
            error = xjinstaller::stream_index_location(index, offset + done, bytes - done, volume_offset, available);
            if (!error)
                error = read_source_device(&reader, reader.source->identity.volume_offset + volume_offset,
                                           static_cast<uint8_t *>(output) + done, available);
            done += available;
        }
    }
    if (!error) error = validate_source(*reader.source);
    if (error) quarantine_source(reader.source);
    __atomic_store_n(&reader.busy, false, __ATOMIC_RELEASE);
    return error;
}
} // namespace installer
