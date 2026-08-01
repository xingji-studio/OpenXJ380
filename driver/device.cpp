#include "device.h"
#include "errno.h"
#include "id_alloc.h"
#include "krlibc.h"
#include "mutex.h"
#include <proto.hpp>
#include <fs/partition.h>
#include <dlinker.h>
#include <mm/uaccess.h>
#include <task/pcb.h>

device_t        device_ctl[256];
id_allocator_t *dev_allocator;
static const size_t BLK_BOUNCE_MAX_BYTES = 0x400000UL;
static const size_t BLK_DIRECT_MIN_BYTES = 0x10000UL;
static uint8_t *blk_cached_bounce[256];
static size_t   blk_cached_bounce_bytes[256];
static mutex_t  blk_cached_bounce_lock[256];
typedef struct {
    uint8_t *buffer;
    size_t   mapped_bytes;
    int      drive;
    bool     using_cache;
} blk_bounce_ctx_t;

static size_t blk_window_bytes(const device_t *device) {
    size_t window = BLK_BOUNCE_MAX_BYTES;
    if (device->max_size != 0 && device->max_size != __UINT64_MAX__) {
        window = MIN(window, (size_t)device->max_size);
    }
    if (window < device->sector_size) {
        window = device->sector_size;
    }
    return window;
}

static uint8_t *blk_alloc_bounce(size_t bytes, size_t *mapped_bytes) {
    size_t pages = PADDING_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    if (pages == 0) {
        pages = 1;
    }
    uint64_t phys = alloc_frames(pages);
    if (phys == 0) {
        *mapped_bytes = 0;
        return NULL;
    }
    page_map_range(get_current_directory(), (uint64_t)driver_phys_to_virt(phys), phys,
                   pages * PAGE_SIZE, KERNEL_PTE_FLAGS);
    *mapped_bytes = pages * PAGE_SIZE;
    return (uint8_t *)driver_phys_to_virt(phys);
}

static void blk_free_bounce(uint8_t *bounce, size_t mapped_bytes) {
    if (bounce == NULL || mapped_bytes == 0) {
        return;
    }
    unmap_page_range(get_current_directory(), (uint64_t)bounce, mapped_bytes);
    free_frames((uint64_t)driver_virt_to_phys((uint64_t)bounce), mapped_bytes / PAGE_SIZE);
}

static bool blk_try_acquire_cached_bounce(int drive, blk_bounce_ctx_t *ctx) {
    if (drive < 0 || drive >= 256) {
        return false;
    }

    if (mutex_trylock(&blk_cached_bounce_lock[drive]) != 0) {
        return false;
    }
    if (blk_cached_bounce[drive] == NULL) {
        blk_cached_bounce[drive] =
            blk_alloc_bounce(BLK_BOUNCE_MAX_BYTES, &blk_cached_bounce_bytes[drive]);
        if (blk_cached_bounce[drive] == NULL) {
            blk_cached_bounce_bytes[drive] = 0;
            mutex_unlock(&blk_cached_bounce_lock[drive]);
            return false;
        }
    }

    ctx->buffer       = blk_cached_bounce[drive];
    ctx->mapped_bytes = blk_cached_bounce_bytes[drive];
    ctx->drive        = drive;
    ctx->using_cache  = true;
    return true;
}

static bool blk_acquire_bounce(int drive, size_t bytes, blk_bounce_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->drive = drive;

    if (bytes <= BLK_BOUNCE_MAX_BYTES && blk_try_acquire_cached_bounce(drive, ctx)) {
        return true;
    }

    ctx->buffer = blk_alloc_bounce(bytes, &ctx->mapped_bytes);
    ctx->using_cache = false;
    return ctx->buffer != NULL;
}

static void blk_release_bounce(blk_bounce_ctx_t *ctx) {
    if (ctx == NULL || ctx->buffer == NULL) {
        return;
    }
    if (ctx->using_cache) {
        mutex_unlock(&blk_cached_bounce_lock[ctx->drive]);
    } else {
        blk_free_bounce(ctx->buffer, ctx->mapped_bytes);
    }
    ctx->buffer       = NULL;
    ctx->mapped_bytes = 0;
    ctx->using_cache  = false;
}

static size_t blk_direct_span_bytes(const void *buffer, size_t max_bytes) {
    if (buffer == NULL || max_bytes == 0) {
        return 0;
    }

    uint64_t base_virt = (uint64_t)buffer;
    uint64_t base_phys = page_virt_to_phys(base_virt);
    if (base_phys == 0) {
        return 0;
    }

    size_t total = 0;
    while (total < max_bytes) {
        uint64_t cur_virt = base_virt + total;
        uint64_t cur_phys = page_virt_to_phys(cur_virt);
        if (cur_phys == 0 || cur_phys != base_phys + total) {
            break;
        }

        size_t page_left = PAGE_SIZE - (cur_virt & (PAGE_SIZE - 1));
        size_t chunk     = MIN(page_left, max_bytes - total);
        total += chunk;
    }

    return total;
}

static bool blk_user_buffer_valid(const void *buffer, size_t length) {
    if (buffer == NULL) {
        return false;
    }
    if (length == 0) {
        return true;
    }

    uint64_t addr = (uint64_t)buffer;
    if (addr > (uint64_t)-1 - (length - 1)) {
        return false;
    }
    if (addr >= KERNEL_AREA_MEM) return true;

    tcb_t current = get_current_task();
    if (current == NULL || current->parent_group == NULL) {
        return false;
    }
    return user_range_mapped(current->parent_group->pagedir, buffer, length);
}

int regist_device(const char *path, device_t vd) {
    int i = id_alloc(dev_allocator);
    if (i == -1) return -1;
    device_ctl[i]         = vd;
    device_ctl[i].vdiskid = i;
    // write_serial_fmt("path: %s id %d\n",path,i);
    errno_t ret;
    if ((ret = devfs_register(path, i)) != EOK) {
        write_serial_fmt("Registers (%s)device error: %d\n", vd.drive_name, ret);
    }else{
        write_serial_fmt("Registers (%s)device success\n", vd.drive_name);
        if (device_ctl[i].type == DEVICE_BLOCK) {
            partition_device_added((size_t)i);
        }
    }
    return i;
}

EXPORT_SYMBOL(regist_device);

void delete_device(int vdiskid) {
    if (vdiskid >= 256) return;
    if (mutex_trylock(&blk_cached_bounce_lock[vdiskid]) == 0) {
        if (blk_cached_bounce[vdiskid] != NULL) {
            blk_free_bounce(blk_cached_bounce[vdiskid], blk_cached_bounce_bytes[vdiskid]);
            blk_cached_bounce[vdiskid]       = NULL;
            blk_cached_bounce_bytes[vdiskid] = 0;
        }
        mutex_unlock(&blk_cached_bounce_lock[vdiskid]);
    }
    device_t dev = device_ctl[vdiskid];
    if (dev.path != NULL)
    {
        devfs_delete(dev.path);
        free(dev.path);
    }
    else if (dev.drive_name[0] != '\0')
    {
        devfs_delete(dev.drive_name);
    }
    id_free(dev_allocator, dev.vdiskid);
    dev.path    = NULL;
    dev.flag    = 0;
    dev.vdiskid = 0;
}
EXPORT_SYMBOL(delete_device);

bool have_vdisk(int drive) {
    int indx = drive;
    if (indx >= 256) { return false; }
    return device_ctl[indx].flag > 0;
}

size_t disk_size(int drive) {
    uint8_t drive1 = drive;
    if (have_vdisk(drive1)) {
        int indx = drive1;
        return device_ctl[indx].size;
    }
    return 0;
}

void *device_mmap(int drive, void *addr, size_t len) {
    if (have_vdisk(drive)) {
        int indx = drive;
        if (device_ctl[indx].map) { return device_ctl[indx].map(drive, addr, len); }
        return NULL;
    }
    return NULL;
}

device_t *get_device(size_t id) {
    if (device_ctl[id].flag == 0) return NULL;
    return &device_ctl[id];
}
size_t rw_device(int drive, size_t lba, uint8_t *buffer, size_t number, int read) {
    int indx = drive;
    if (indx >= 256){ return 0;}
    if (device_ctl[indx].flag == 0){return 0;}

    size_t   sector_size = device_ctl[indx].sector_size;
    if (sector_size == 0 || (number != 0 && sector_size > (size_t)-1 / number)) {
        return 0;
    }
    size_t   total_size  = number * sector_size;
    if (total_size > (size_t)-1 - (PAGE_SIZE - 1)) {
        return 0;
    }
    size_t   page_size   = PADDING_UP(total_size, PAGE_SIZE) / PAGE_SIZE;
    uint64_t phys        = alloc_frames(page_size);
    if (phys == 0) {
        return 0;
    }
    page_map_range(get_current_directory(), (uint64_t)driver_phys_to_virt(phys), phys,
                   page_size * PAGE_SIZE, PTE_PRESENT | PTE_WRITEABLE);
    uint8_t *kbuf = (uint8_t*)driver_phys_to_virt(phys);
    size_t   ret  = 0;
    if (read) {
        // write_serial_fmt("DRIVE %d\n",drive);
        ret = device_ctl[indx].read(drive, kbuf, number, lba);
        memcpy(buffer, kbuf, total_size);
    } else {
        memcpy(kbuf, buffer, total_size);
        ret = device_ctl[indx].write(drive, kbuf, number, lba);
    }
    blk_free_bounce(kbuf, page_size * PAGE_SIZE);
    return ret;
}
size_t device_read(size_t lba, size_t number, void *buffer, int drive) {
    if (have_vdisk(drive)) {
        if (device_ctl[drive].type == DEVICE_STREAM) {
            return device_ctl[drive].read(drive, (uint8_t*)buffer, number, lba);
        }
        if (device_ctl[drive].sector_size == 0 ||
            (number != 0 && device_ctl[drive].sector_size > (size_t)-1 / number)) {
            return 0;
        }
        size_t ret_size = 0;
        for (size_t i = 0; i < number; i += SECTORS_ONCE) {
            int sectors  = ((number - i) >= SECTORS_ONCE) ? SECTORS_ONCE : (number - i);
            ret_size    += rw_device(drive, lba + i,
                                     (uint8_t *)((uint64_t)buffer + i * device_ctl[drive].sector_size),
                                     sectors, 1);
        }
        return ret_size;
    }
    return 0;
}
size_t device_write(size_t lba, size_t number, const void *buffer, int drive) {
    if (have_vdisk(drive)) {
        if (device_ctl[drive].sector_size == 0 ||
            (number != 0 && device_ctl[drive].sector_size > (size_t)-1 / number)) {
            write_serial_fmt("device_write: reject drive=%d name=%s lba=%zu sectors=%zu sector_size=%zu\n",
                             drive, device_ctl[drive].drive_name, lba, number, device_ctl[drive].sector_size);
            return 0;
        }
        size_t ret_size = 0;
        for (size_t i = 0; i < number; i += SECTORS_ONCE) {
            int sectors  = ((number - i) >= SECTORS_ONCE) ? SECTORS_ONCE : (number - i);
            size_t got = rw_device(drive, lba + i,
                                   (uint8_t *)((uint64_t)buffer + i * device_ctl[drive].sector_size),
                                   sectors, 0);
            ret_size += got;
            if (got != (size_t)sectors) {
                write_serial_fmt("device_write: short drive=%d name=%s lba=%zu sectors=%d got=%zu\n",
                                 drive, device_ctl[drive].drive_name, lba + i, sectors, got);
                break;
            }
        }
        return ret_size;
    }
    write_serial_fmt("device_write: missing drive=%d lba=%zu sectors=%zu\n", drive, lba, number);
    return 0;
}

size_t blk_device_read(int drive, void *buffer, size_t offset, size_t length) {
    if (!have_vdisk(drive) || length == 0) {
        return 0;
    }

    device_t device = device_ctl[drive];
    if (device.type == DEVICE_STREAM) {
        return device.read(drive, (uint8_t *)buffer, length, offset);
    }
    if (device.sector_size == 0 || device.read == NULL) {
        return (uint64_t)-1;
    }
    blk_bounce_ctx_t bounce_ctx;
    if (!blk_acquire_bounce(drive, blk_window_bytes(&device), &bounce_ctx)) {
        return (uint64_t)-1;
    }
    uint8_t *bounce = bounce_ctx.buffer;

    const uint64_t sector_size    = device.sector_size;
    const uint64_t max_sec        = MAX(blk_window_bytes(&device) / sector_size, 1);
    uint8_t       *dest           = (uint8_t *)buffer;
    uint64_t       sector         = offset / sector_size;
    uint64_t       offset_in_block = offset % sector_size;
    uint64_t       remaining      = length;
    uint64_t       mid_secs       = 0;
    uint64_t       total_read     = 0;
    size_t         status         = (uint64_t)-1;

    if (offset_in_block != 0) {
        uint64_t head = MIN(sector_size - offset_in_block, remaining);
        if (device.read(device.vdiskid, bounce, 1, sector) != 1) {
            goto out;
        }
        memcpy(dest, bounce + offset_in_block, head);

        dest += head;
        remaining -= head;
        total_read += head;
        sector++;
    }

    while (remaining >= sector_size) {
        size_t direct_limit = MIN((size_t)remaining, blk_window_bytes(&device));
        size_t direct_bytes = blk_direct_span_bytes(dest, direct_limit);
        direct_bytes -= direct_bytes % sector_size;
        if (direct_bytes < sector_size) {
            break;
        }
        if (direct_bytes < BLK_DIRECT_MIN_BYTES && direct_bytes != direct_limit) {
            break;
        }

        uint64_t direct_secs = direct_bytes / sector_size;
        if (device.read(device.vdiskid, dest, direct_secs, sector) != direct_secs) {
            goto out;
        }

        dest += direct_bytes;
        remaining -= direct_bytes;
        total_read += direct_bytes;
        sector += direct_secs;
    }

    mid_secs = remaining / sector_size;
    if (mid_secs > 0) {
        uint64_t bn           = MIN(mid_secs, max_sec);
        while (mid_secs > 0) {
            uint64_t n = MIN(mid_secs, bn);
            if (device.read(device.vdiskid, bounce, n, sector) != n) {
                goto out;
            }

            uint64_t bytes = n * sector_size;
            memcpy(dest, bounce, bytes);
            dest += bytes;
            remaining -= bytes;
            total_read += bytes;
            sector += n;
            mid_secs -= n;
        }
    }

    if (remaining > 0) {
        if (device.read(device.vdiskid, bounce, 1, sector) != 1) {
            goto out;
        }
        memcpy(dest, bounce, remaining);
        total_read += remaining;
    }

    status = total_read;

out:
    blk_release_bounce(&bounce_ctx);
    return status;
}

size_t blk_device_write(device_t device, const void *buffer, size_t offset, size_t length) {
    if (length == 0) {
        return 0;
    }
    if (!blk_user_buffer_valid(buffer, length)) {
        write_serial_fmt("blk_device_write: reject unmapped buffer=%#llx len=%#llx thread=%s\n",
                         (unsigned long long)(uint64_t)buffer, (unsigned long long)length,
                         get_current_task() ? get_current_task()->name : "<none>");
        return (uint64_t)-1;
    }
    if (device.type == DEVICE_STREAM) {
        return device.write(device.vdiskid, (uint8_t *)buffer, length, offset);
    }
    if (device.sector_size == 0 || device.write == NULL) {
        return (uint64_t)-1;
    }
    blk_bounce_ctx_t bounce_ctx;
    if (!blk_acquire_bounce(device.vdiskid, blk_window_bytes(&device), &bounce_ctx)) {
        return (uint64_t)-1;
    }
    uint8_t *bounce = bounce_ctx.buffer;

    const uint64_t sector_size     = device.sector_size;
    const uint64_t max_sec         = MAX(blk_window_bytes(&device) / sector_size, 1);
    const uint8_t *src             = (const uint8_t *)buffer;
    uint64_t       sector          = offset / sector_size;
    uint64_t       offset_in_block = offset % sector_size;
    uint64_t       remaining       = length;
    uint64_t       mid_secs        = 0;
    uint64_t       total_written   = 0;
    size_t         status          = (uint64_t)-1;

    if (offset_in_block != 0) {
        if (device.read == NULL) {
            goto out;
        }

        uint64_t head = MIN(sector_size - offset_in_block, remaining);
        if (device.read(device.vdiskid, bounce, 1, sector) != 1) {
            goto out;
        }
        memcpy(bounce + offset_in_block, src, head);
        if (device.write(device.vdiskid, bounce, 1, sector) != 1) {
            goto out;
        }

        src += head;
        remaining -= head;
        total_written += head;
        sector++;
    }

    while (remaining >= sector_size) {
        size_t direct_limit = MIN((size_t)remaining, blk_window_bytes(&device));
        size_t direct_bytes = blk_direct_span_bytes(src, direct_limit);
        direct_bytes -= direct_bytes % sector_size;
        if (direct_bytes < sector_size) {
            break;
        }
        if (direct_bytes < BLK_DIRECT_MIN_BYTES && direct_bytes != direct_limit) {
            break;
        }

        uint64_t direct_secs = direct_bytes / sector_size;
        if (device.write(device.vdiskid, (uint8_t *)src, direct_secs, sector) != direct_secs) {
            goto out;
        }

        src += direct_bytes;
        remaining -= direct_bytes;
        total_written += direct_bytes;
        sector += direct_secs;
    }

    mid_secs = remaining / sector_size;
    if (mid_secs > 0) {
        uint64_t bn           = MIN(mid_secs, max_sec);
        while (mid_secs > 0) {
            uint64_t n     = MIN(mid_secs, bn);
            uint64_t bytes = n * sector_size;
            memcpy(bounce, src, bytes);
            if (device.write(device.vdiskid, bounce, n, sector) != n) {
                goto out;
            }

            src += bytes;
            remaining -= bytes;
            total_written += bytes;
            sector += n;
            mid_secs -= n;
        }
    }

    if (remaining > 0) {
        if (device.read == NULL) {
            goto out;
        }

        if (device.read(device.vdiskid, bounce, 1, sector) != 1) {
            goto out;
        }
        memcpy(bounce, src, remaining);
        if (device.write(device.vdiskid, bounce, 1, sector) != 1) {
            goto out;
        }
        total_written += remaining;
    }

    status = total_written;

out:
    blk_release_bounce(&bounce_ctx);
    return status;
}


int device_manager_init() {
    for (size_t i = 0; i < 256; i++) {
        device_ctl[i].flag = 0; // 设置为未使用
        blk_cached_bounce[i] = NULL;
        blk_cached_bounce_bytes[i] = 0;
        mutex_create(&blk_cached_bounce_lock[i], true);
    }
    dev_allocator = id_allocator_create(256);
    return 0;
}
