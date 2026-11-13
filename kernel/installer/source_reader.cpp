// SPDX-License-Identifier: Apache-2.0
#include <installer/source_reader.h>
#include <fs/vfs/vfs.h>
#include <task/pcb.h>
#include <krlibc.h>

namespace installer
{
int read_stream_source(void *opaque, uint64_t offset, void *output, size_t bytes)
{
    if (!opaque || !output || !bytes) return -22;
    auto &reader = *static_cast<SourceReadContext *>(opaque);
    if (!reader.source || !reader.index) return -22;
    auto &source = *reader.source;
    const auto &index = *reader.index;
    if (offset > index.data_bytes || bytes > index.data_bytes - offset) return -22;
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_KERNEL_LEVEL || task->status == DEATH || task->status == OUT)
        return -1;
    bool expected = false;
    // One admitted installation uses this bounded scratch buffer. Recursive or
    // competing readers fail; no spin/wait or scheduler gate covers source I/O.
    if (!__atomic_compare_exchange_n(&reader.busy, &expected, true, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return -16;
    int error = validate_source(source);
    const auto &identity = source.identity;
    uint64_t block = identity.physical_block_size;
    if (!error && (!source.read_only_stream || source.reference.writable ||
                   !block || block > sizeof(reader.bounce) || block & (block - 1))) error = -95;
    unsigned prior_privilege = task->ime_storage_depth;
    if (!error && !prior_privilege) task->ime_storage_depth = 1;
    size_t done = 0;
    while (!error && done < bytes)
    {
        uint64_t volume_offset = 0;
        size_t available = 0;
        error = xjinstaller::stream_index_location(index, offset + done, bytes - done,
                                                   volume_offset, available);
        if (error) break;
        uint64_t physical = identity.volume_offset + volume_offset;
        uint64_t aligned = physical & ~(block - 1), within = physical - aligned;
        size_t amount = available < sizeof(reader.bounce) - within ? available :
                                                                   sizeof(reader.bounce) - size_t(within);
        size_t request = (size_t(within) + amount + size_t(block) - 1) & ~(size_t(block) - 1);
        if (aligned > identity.physical_bytes || request > identity.physical_bytes - aligned ||
            vfs_read(source.reference.node, reader.bounce, aligned, request) != request)
        {
            error = -5;
            break;
        }
        memcpy(static_cast<uint8_t *>(output) + done, reader.bounce + within, amount);
        done += amount;
    }
    task->ime_storage_depth = prior_privilege;
    if (error) quarantine_source(&source);
    __atomic_store_n(&reader.busy, false, __ATOMIC_RELEASE);
    return error;
}
} // namespace installer
