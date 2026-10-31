#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// Internal FatFs port hooks. Caller owns the ordinary FAT recursive mutex.
// Private windows bypass the normal read-ahead cache and authenticate every
// request against their retained whole-device identity and parent lease.
bool fatfs_esp_private_drive(unsigned drive);
int fatfs_esp_disk_status(unsigned drive);
int fatfs_esp_disk_read(unsigned drive, void *buffer, uint64_t sector, unsigned count);
int fatfs_esp_disk_write(unsigned drive, const void *buffer, uint64_t sector, unsigned count);
int fatfs_esp_disk_sync(unsigned drive);
uint32_t fatfs_esp_disk_sectors(unsigned drive);
void fatfs_disk_cache_reset(unsigned drive);

// A private factory session preallocates these scratch slabs before any I/O.
// handled distinguishes exhausted private storage from normal heap fallback.
void *fatfs_esp_memalloc(unsigned bytes, bool *handled);
bool fatfs_esp_memfree(void *buffer);
