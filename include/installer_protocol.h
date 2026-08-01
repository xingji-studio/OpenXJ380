#pragma once

typedef unsigned char      xj380_inst_u8;
typedef unsigned short     xj380_inst_u16;
typedef unsigned int       xj380_inst_u32;
typedef unsigned long long xj380_inst_u64;
typedef long long          xj380_inst_i64;

#define XJ380_INSTALLER_MAX_DISKS      32
#define XJ380_INSTALLER_DISK_NAME_LEN  48
#define XJ380_INSTALLER_STAGE_LEN      96
#define XJ380_INSTALLER_DETAIL_LEN     192
#define XJ380_INSTALLER_QUEUE_ITEMS     16
#define XJ380_INSTALLER_QUEUE_LEN      128
#define XJ380_INSTALLER_CHECK_ITEMS     12
#define XJ380_INSTALLER_RESCUE_ITEMS    16
#define XJ380_INSTALLER_LOG_LINES       24
#define XJ380_INSTALLER_LOG_LEN        160
#define XJ380_INSTALLER_BOOT_MAGIC     0x584A33383049534FULL
#define XJ380_INSTALLER_PAK_MAGIC      "XJPAK10"
#define XJ380_INSTALLER_PAK_MAGIC_SIZE 8

#define XJ380_INSTALLER_DISK_FLAG_WRITABLE      (1ULL << 0)
#define XJ380_INSTALLER_DISK_FLAG_BOOT_MEDIA    (1ULL << 1)
#define XJ380_INSTALLER_DISK_FLAG_SLICE         (1ULL << 2)
#define XJ380_INSTALLER_DISK_FLAG_SECTOR_512    (1ULL << 3)
#define XJ380_INSTALLER_DISK_FLAG_INSTALLABLE   (1ULL << 4)

#define XJ380_INSTALLER_MODE_FRESH          0
#define XJ380_INSTALLER_MODE_REPAIR_BOOT    1
#define XJ380_INSTALLER_MODE_KEEP_USERS     2
#define XJ380_INSTALLER_MODE_DEVELOPER      3

#define XJ380_INSTALLER_CHECK_OK      0
#define XJ380_INSTALLER_CHECK_WARN    1
#define XJ380_INSTALLER_CHECK_ERROR   2

#define XJ380_INSTALLER_RESCUE_REBUILD_BOOT 1
#define XJ380_INSTALLER_RESCUE_CHECK_DISK   2
#define XJ380_INSTALLER_RESCUE_VIEW_DISK    3
#define XJ380_INSTALLER_RESCUE_OPEN_TERM    4
#define XJ380_INSTALLER_RESCUE_VIEW_LOG     5

#define XJ380_INSTALLER_COMPONENT_LINUX_COMPAT  (1ULL << 0)
#define XJ380_INSTALLER_COMPONENT_PYTHON        (1ULL << 1)
#define XJ380_INSTALLER_COMPONENT_LLVM_CLANG    (1ULL << 2)
#define XJ380_INSTALLER_COMPONENT_GCC           (1ULL << 3)
#define XJ380_INSTALLER_COMPONENT_DEFAULT       (XJ380_INSTALLER_COMPONENT_LINUX_COMPAT | \
                                                 XJ380_INSTALLER_COMPONENT_PYTHON | \
                                                 XJ380_INSTALLER_COMPONENT_LLVM_CLANG | \
                                                 XJ380_INSTALLER_COMPONENT_GCC)
#define XJ380_INSTALLER_COMPONENT_ALL           XJ380_INSTALLER_COMPONENT_DEFAULT

#define XJ380_PAK_ENTRY_FILE 1
#define XJ380_PAK_ENTRY_DIR  2

typedef struct {
    char           magic[XJ380_INSTALLER_PAK_MAGIC_SIZE];
    xj380_inst_u64 entry_count;
} xj380_pak_header;

typedef struct {
    xj380_inst_u16 path_len;
    xj380_inst_u16 type;
    xj380_inst_u32 mode;
    xj380_inst_u64 size;
} xj380_pak_entry_header;

typedef struct {
    xj380_inst_u32 id;
    xj380_inst_u32 sector_size;
    xj380_inst_u64 size_bytes;
    xj380_inst_u64 flags;
    char           name[XJ380_INSTALLER_DISK_NAME_LEN];
} xj380_installer_disk;

typedef struct {
    xj380_inst_u32 count;
    xj380_inst_u32 reserved;
    xj380_installer_disk disks[XJ380_INSTALLER_MAX_DISKS];
} xj380_installer_disk_list;

typedef struct {
    xj380_inst_u32 status;
    xj380_inst_u32 code;
    char           title[64];
    char           detail[XJ380_INSTALLER_DETAIL_LEN];
} xj380_installer_check_item;

typedef struct {
    xj380_inst_u32 disk_id;
    xj380_inst_u32 mode;
    xj380_inst_u32 language;
    xj380_inst_u32 reserved;
    xj380_inst_u64 components;
} xj380_installer_start_options;

typedef struct {
    xj380_inst_u32 disk_id;
    xj380_inst_u32 mode;
    xj380_inst_u32 item_count;
    xj380_inst_u32 can_continue;
    xj380_inst_u64 components;
    xj380_inst_u64 payload_bytes;
    xj380_inst_u64 required_bytes;
    xj380_inst_u64 target_bytes;
    xj380_inst_u64 efi_first_lba;
    xj380_inst_u64 efi_last_lba;
    xj380_installer_check_item items[XJ380_INSTALLER_CHECK_ITEMS];
} xj380_installer_precheck;

typedef enum {
    XJ380_INSTALLER_IDLE = 0,
    XJ380_INSTALLER_RUNNING,
    XJ380_INSTALLER_DONE,
    XJ380_INSTALLER_FAILED,
} xj380_installer_state;

typedef struct {
    xj380_inst_u32 state;
    xj380_inst_u32 percent;
    xj380_inst_i64 result;
    char           stage[XJ380_INSTALLER_STAGE_LEN];
    char           detail[XJ380_INSTALLER_DETAIL_LEN];
    xj380_inst_u32 queue_index;
    xj380_inst_u32 queue_total;
    xj380_inst_u32 queue_count;
    xj380_inst_u32 queue_reserved;
    char           queue_items[XJ380_INSTALLER_QUEUE_ITEMS][XJ380_INSTALLER_QUEUE_LEN];
    xj380_inst_u32 mode;
    xj380_inst_u32 stage_percent;
    xj380_inst_u32 total_percent;
    xj380_inst_u32 small_file_count;
    xj380_inst_u32 large_file_count;
    xj380_inst_u32 copied_small_file_count;
    xj380_inst_u32 copied_large_file_count;
    xj380_inst_u64 bytes_per_second;
    xj380_inst_u64 eta_seconds;
    xj380_inst_u64 copied_bytes;
    xj380_inst_u64 total_bytes;
    xj380_inst_u64 current_file_bytes;
    xj380_inst_u64 current_file_size;
} xj380_installer_progress;

typedef struct {
    xj380_inst_u32 status;
    xj380_inst_u32 code;
    char           title[64];
    char           detail[XJ380_INSTALLER_DETAIL_LEN];
} xj380_installer_rescue_item;

typedef struct {
    xj380_inst_i64 result;
    xj380_inst_u32 item_count;
    xj380_inst_u32 reserved;
    xj380_installer_rescue_item items[XJ380_INSTALLER_RESCUE_ITEMS];
} xj380_installer_rescue_result;

typedef struct {
    xj380_inst_u32 count;
    xj380_inst_u32 reserved;
    char           lines[XJ380_INSTALLER_LOG_LINES][XJ380_INSTALLER_LOG_LEN];
} xj380_installer_log;
