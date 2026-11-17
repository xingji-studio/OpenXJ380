// SPDX-License-Identifier: Apache-2.0
#include "../../driver/fs/ntfs/mkfs.h"
#include <block_access.h>
#include <device.h>
#include <errno.h>
#include <fs/fatfs/ff.h>
#include <fs/vfs/vfs.h>
#include <ioctl.h>
#include <krlibc.h>
#include <rng.h>
#include <rtc.h>
#include <task/pcb.h>

namespace {
struct Context
{
    BlockAccessWindow       window;
    xjntfs::mkfs::Workspace workspace;
    xjntfs::mkfs::Status    status;
    bool                    failed;
};

class StorageScope {
    tcb_t task;

  public:
    StorageScope() : task(get_current_task())
    {
        if (task) ++task->ime_storage_depth;
    }
    ~StorageScope()
    {
        if (task) --task->ime_storage_depth;
    }
};

bool valid(Context *context, uint64_t offset, size_t bytes)
{
    if (context->failed) return false;
    const auto &w = context->window;
    BlockExtent actual{};
    int         error = block_access_resolve_node(w.node, &actual);
    if (error || actual.device != w.whole.device || actual.physical_device != w.whole.physical_device ||
        actual.generation != w.whole.generation || actual.physical_generation != w.whole.physical_generation ||
        actual.start != w.whole.start || actual.bytes != w.whole.bytes || offset > w.bytes ||
        bytes > w.bytes - offset || block_access_validate_owned_exclusive(w.lease, w.whole, w.offset, w.bytes))
    {
        context->failed = true;
        block_access_poison(w.lease);
        return false;
    }
    return true;
}

bool finish(Context *context, bool success)
{
    if (!success)
    {
        context->failed = true;
        block_access_poison(context->window.lease);
    }
    return success;
}

bool read(void *opaque, uint64_t offset, void *p, size_t bytes)
{
    auto        *c = static_cast<Context *>(opaque);
    StorageScope scope;
    return valid(c, offset, bytes) && finish(c, vfs_read(c->window.node, p, c->window.offset + offset, bytes) == bytes);
}

bool write(void *opaque, uint64_t offset, const void *p, size_t bytes)
{
    auto        *c = static_cast<Context *>(opaque);
    StorageScope scope;
    return valid(c, offset, bytes) &&
           finish(c, vfs_write(c->window.node, const_cast<void *>(p), c->window.offset + offset, bytes) == bytes);
}

bool flush(void *opaque)
{
    auto        *c = static_cast<Context *>(opaque);
    StorageScope scope;
    return valid(c, 0, c->window.bytes) && finish(c, vfs_ioctl(c->window.node, IOBLKSYNC, nullptr) == 0);
}
} // namespace

int installer_format_ntfs_window(const BlockAccessWindow *window)
{
    if (!window || !window->node || window->offset % 512 || window->bytes % 512) return -EINVAL;
    int error = block_access_validate_owned_exclusive(window->lease, window->whole, window->offset, window->bytes);
    if (error) return error;
    auto *device = get_device(window->whole.physical_device);
    if (!device || device->sector_size != 512) return -EOPNOTSUPP;
    xjntfs::mkfs::Options options{};
    if (!get_identifier_random_bytes(&options.serial, sizeof(options.serial)) || !options.serial) return -EIO;
    options.time_seconds  = realtime_ns() / 1000000000ULL;
    options.partition_lba = window->offset / 512;
    options.uppercase     = ff_wtoupper;
    auto *context         = static_cast<Context *>(calloc(1, sizeof(Context)));
    if (!context) return -ENOMEM;
    context->window = *window;
    xjntfs::mkfs::Device io{context, window->bytes, read, write, flush};
    error = xjntfs::mkfs::format(io, options, context->workspace, context->status);
    if (error && context->status.io_started) block_access_poison(window->lease);
    if (!context->failed && !(error && context->status.io_started)) free(context);
    return error;
}
