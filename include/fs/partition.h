#pragma once

#define GPT_HEADER_SIGNATURE  "EFI PART"
#define MAX_PARTITIONS_NUM    128
#define MBR_MAX_PARTITION_NUM 4

#define PARTITION_TYPE_GPT     0xC12A7328
#define PARTITION_TYPE_MBR     0xEBD0A0A2
#define PARTITION_TYPE_UNKNOWN 0xFFFFFFFF

#include "krlibc.h"

struct GPT_DPT {
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} __attribute__((packed));

struct GPT_DPTE {
    uint8_t  partition_type_guid[16];
    uint8_t  unique_partition_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t partition_name[36];
} __attribute__((packed));

struct MBR_DPTE {
    uint8_t  flags;
    uint8_t  start_head;
    uint16_t start_sector : 6,
        start_cylinder    : 10;
    uint8_t  type;
    uint8_t  end_head;
    uint16_t end_sector : 6,
        end_cylinder    : 10;
    uint32_t start_lba;
    uint32_t sectors_limit;
} __attribute__((packed));

struct MBR_DPT {
    uint8_t         bs_reserved[446];
    struct MBR_DPTE dpte[4];
    uint16_t        bs_trail_sig;
} __attribute__((packed));

typedef struct partition {
    size_t vdisk_id;
    size_t starting_lba;
    size_t ending_lba;
    size_t sector_size;
    enum {
        MBR = 1,
        GPT,
        PRAW,
    } type;
    bool     is_used;
    uint16_t partition_name[36];
    uint8_t  partition_type_guid[16];
    uint8_t  unique_partition_guid[16];
    uint8_t  disk_guid[16];
} partition_t;

void partition_init();
extern "C" void partition_device_added(size_t vdisk_id);
void partition_rescan_device(size_t vdisk_id, bool auto_mount);
int  partition_find_first_for_disk(size_t vdisk_id);
