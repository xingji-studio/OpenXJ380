/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "fs/fatfs/diskio.h" /* Declarations of disk functions */
#include "fs/fatfs/ff.h"     /* Obtains integer types */
#include "krlibc.h"
#include "proto.hpp"
#include <ioctl.h>
#include <fs/fatfs/private.h>

vfs_node_t fatfs_get_node_by_number(int number);
extern bool ahci_is_qemu_environment();

static const size_t FATFS_READAHEAD_BYTES = 0x80000UL;

typedef struct {
    byte  *data;
    size_t size_bytes;
    LBA_t  start_sector;
    uint   sector_count;
    bool   valid;
} fatfs_read_cache_t;

static fatfs_read_cache_t fatfs_read_cache[10];

static bool fatfs_readahead_allowed()
{
    return ahci_is_qemu_environment();
}

static bool fatfs_cache_covers(const fatfs_read_cache_t *cache, LBA_t sector, uint count)
{
    return cache->valid && sector >= cache->start_sector &&
           (uint64_t)sector + count <= (uint64_t)cache->start_sector + cache->sector_count;
}

static void fatfs_cache_invalidate(byte pdrv)
{
    if (pdrv < 10) {
        fatfs_read_cache[pdrv].valid = false;
    }
}

// Original Apache-2.0 port hook. A reused private drive must never observe
// a prior normal drive's cached sectors, or publish them to a later normal use.
void fatfs_disk_cache_reset(unsigned drive)
{
    if (drive < 10) fatfs_cache_invalidate(byte(drive));
}

static bool fatfs_cache_ensure(byte pdrv)
{
    if (pdrv >= 10) return false;
    fatfs_read_cache_t *cache = &fatfs_read_cache[pdrv];
    if (cache->data != NULL) return true;

    size_t   pages = PADDING_UP(FATFS_READAHEAD_BYTES, PAGE_SIZE) / PAGE_SIZE;
    uint64_t phys  = alloc_frames(pages);
    if (phys == 0) return false;

    cache->data = (byte *)phys_to_virt(phys);
    if (cache->data == NULL) return false;

    cache->size_bytes = FATFS_READAHEAD_BYTES;
    cache->valid      = false;
    return true;
}

static DRESULT fatfs_cache_fill(byte pdrv, LBA_t sector, uint min_count)
{
    vfs_node_t node = fatfs_get_node_by_number(pdrv);
    if (node == NULL || !fatfs_cache_ensure(pdrv)) return RES_PARERR;

    fatfs_read_cache_t *cache         = &fatfs_read_cache[pdrv];
    uint64_t            total_sectors = node->size / 0x200;
    uint                cache_sectors = cache->size_bytes / 0x200;
    if (cache_sectors == 0 || sector >= total_sectors) return RES_PARERR;

    uint64_t remaining = total_sectors - sector;
    uint     to_read   = cache_sectors;
    if (to_read < min_count) to_read = min_count;
    if (to_read > remaining) to_read = (uint)remaining;

    size_t bytes = (size_t)to_read * 0x200;
    size_t got   = vfs_read(node, cache->data, sector * 0x200, bytes);
    if (got != bytes) {
        cache->valid = false;
        return RES_ERROR;
    }

    cache->start_sector = sector;
    cache->sector_count = to_read;
    cache->valid        = true;
    return RES_OK;
}
/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status(byte pdrv /* Physical drive nmuber to identify the drive */
)
{
    if (fatfs_esp_private_drive(pdrv)) return fatfs_esp_disk_status(pdrv) ? STA_NOINIT : 0;
    DSTATUS stat = STA_NOINIT;
    // int result;
    if (fatfs_get_node_by_number(pdrv)) stat &= ~STA_NOINIT;
    return stat;
}

/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize(byte pdrv /* Physical drive nmuber to identify the drive */
)
{
    if (fatfs_esp_private_drive(pdrv)) return fatfs_esp_disk_status(pdrv) ? STA_NOINIT : 0;
    DSTATUS stat = STA_NOINIT;
    // int result;
    if (fatfs_get_node_by_number(pdrv)) stat &= ~STA_NOINIT;
    return stat;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read(byte  pdrv,   /* Physical drive nmuber to identify the drive */
                  byte *buff,   /* Data buffer to store read data */
                  LBA_t sector, /* Start sector in LBA */
                  uint  count   /* Number of sectors to read */
)
{
    // Private factory I/O always goes to the authenticated raw window. It
    // bypasses read-ahead completely, including cache hits from prior slots.
    if (fatfs_esp_private_drive(pdrv))
    {
        fatfs_cache_invalidate(pdrv);
        return fatfs_esp_disk_read(pdrv, buff, sector, count) ? RES_ERROR : RES_OK;
    }
    DRESULT res = RES_PARERR;
    vfs_node_t node = fatfs_get_node_by_number(pdrv);
    if (!node || buff == NULL || count == 0) return RES_PARERR;

    fatfs_read_cache_t *cache = pdrv < 10 ? &fatfs_read_cache[pdrv] : NULL;
    if (fatfs_readahead_allowed() && cache != NULL && count <= 8 && fatfs_cache_ensure(pdrv)) {
        if (!fatfs_cache_covers(cache, sector, count)) {
            if (fatfs_cache_fill(pdrv, sector, count) != RES_OK) return RES_ERROR;
        }
        size_t offset = (size_t)(sector - cache->start_sector) * 0x200;
        memcpy(buff, cache->data + offset, count * 0x200);
        return RES_OK;
    }

    fatfs_cache_invalidate(pdrv);
    size_t expected = (size_t)count * 0x200;
    size_t got      = vfs_read(node, buff, sector * 0x200, expected);
    if (got != expected) {
        write_serial_fmt("FATFS: disk_read failed pdrv=%u sector=%llu count=%u expected=%zu got=%zu\n", pdrv,
                         (uint64_t)sector, count, expected, got);
        return RES_ERROR;
    }
    res = RES_OK;
    return res;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write(byte        pdrv,   /* Physical drive nmuber to identify the drive */
                   const byte *buff,   /* Data to be written */
                   LBA_t       sector, /* Start sector in LBA */
                   uint        count   /* Number of sectors to write */
)
{
    if (fatfs_esp_private_drive(pdrv))
    {
        fatfs_cache_invalidate(pdrv);
        return fatfs_esp_disk_write(pdrv, buff, sector, count) ? RES_ERROR : RES_OK;
    }
    DRESULT res = RES_PARERR;
    if (!fatfs_get_node_by_number(pdrv)) return RES_PARERR;
    fatfs_cache_invalidate(pdrv);
    if (vfs_write(fatfs_get_node_by_number(pdrv), (void *)buff, sector * 0x200, count * 0x200) != count * 0x200) {
        return RES_ERROR;
    }
    res = RES_OK;
    return res;
}

#endif

extern vfs_node_t drive_number_mapping[10];

u32 disk_size(byte drive)
{
    if (fatfs_esp_private_drive(drive)) return fatfs_esp_disk_sectors(drive);
    if (drive >= 10 || drive_number_mapping[drive] == NULL) return 0;
    return (u32)(drive_number_mapping[drive]->size / 512);
}

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(byte  pdrv, /* Physical drive nmuber (0..) */
                   byte  cmd,  /* Control code */
                   void *buff  /* Buffer to send/receive control data */
)
{
    if (fatfs_esp_private_drive(pdrv))
    {
        if (fatfs_esp_disk_status(pdrv)) return RES_ERROR;
        switch (cmd)
        {
        case GET_SECTOR_SIZE: if (!buff) return RES_PARERR; *(u16 *)buff = 512; return RES_OK;
        case GET_SECTOR_COUNT: if (!buff) return RES_PARERR; *(LBA_t *)buff = fatfs_esp_disk_sectors(pdrv); return RES_OK;
        case GET_BLOCK_SIZE: if (!buff) return RES_PARERR; *(u32 *)buff = 1; return RES_OK;
        case CTRL_SYNC: return fatfs_esp_disk_sync(pdrv) ? RES_ERROR : RES_OK;
        default: return RES_PARERR;
        }
    }
    // DRESULT res;
    // int result;

    switch (cmd)
    {
    case GET_SECTOR_SIZE: *(u16 *)buff = 512; return RES_OK;
    case GET_SECTOR_COUNT: *(u32 *)buff = disk_size(pdrv); return RES_OK;
    case GET_BLOCK_SIZE: *(u16 *)buff = 0; return RES_OK;
    case CTRL_SYNC:
    {
        vfs_node_t node = fatfs_get_node_by_number(pdrv);
        if (!node) return RES_PARERR;
        return vfs_ioctl(node, IOBLKSYNC, NULL) == 0 ? RES_OK : RES_ERROR;
    }
    default: break;
    }

    return RES_PARERR;
}
