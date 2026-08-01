// IDE 控制器驱动头文件
#pragma once

#include <stdint.h>
#include <device.h>

// IDE 端口定义
#define IDE_PRIMARY_DATA_PORT   0x1F0
#define IDE_PRIMARY_ERROR_PORT  0x1F1
#define IDE_PRIMARY_SECCOUNT_PORT 0x1F2
#define IDE_PRIMARY_LBA_LOW_PORT  0x1F3
#define IDE_PRIMARY_LBA_MID_PORT  0x1F4
#define IDE_PRIMARY_LBA_HIGH_PORT 0x1F5
#define IDE_PRIMARY_DRIVE_PORT    0x1F6
#define IDE_PRIMARY_STATUS_PORT   0x1F7
#define IDE_PRIMARY_CMD_PORT      0x1F7

#define IDE_SECONDARY_DATA_PORT   0x170
#define IDE_SECONDARY_ERROR_PORT  0x171
#define IDE_SECONDARY_SECCOUNT_PORT 0x172
#define IDE_SECONDARY_LBA_LOW_PORT  0x173
#define IDE_SECONDARY_LBA_MID_PORT  0x174
#define IDE_SECONDARY_LBA_HIGH_PORT 0x175
#define IDE_SECONDARY_DRIVE_PORT    0x176
#define IDE_SECONDARY_STATUS_PORT   0x177
#define IDE_SECONDARY_CMD_PORT      0x177

#define IDE_CONTROL_PORT  0x3F6

// IDE 命令
#define IDE_CMD_READ_SECTORS  0x20
#define IDE_CMD_WRITE_SECTORS 0x30
#define IDE_CMD_IDENTIFY      0xEC
#define IDE_CMD_CACHE_FLUSH   0xE7

// IDE 状态寄存器位
#define IDE_STATUS_BSY  0x80
#define IDE_STATUS_DRQ  0x08
#define IDE_STATUS_DF   0x20
#define IDE_STATUS_ERR  0x01

// IDE 设备选择
#define IDE_DEVICE_MASTER 0xA0
#define IDE_DEVICE_SLAVE  0xB0

// IDE 设备类型
#define IDE_DEVICE_ATA    0x00
#define IDE_DEVICE_ATAPI  0x80

typedef struct ide_device {
    uint8_t  exists;
    uint8_t  is_master;
    uint8_t  is_primary;
    uint8_t  device_type;
    uint32_t sectors;
    uint32_t sector_size;
    char     model[41];
    char     serial[21];
    int      drive_id;  // 注册后的设备 ID
} ide_device_t;

// 函数声明
#ifdef __cplusplus
extern "C" {
#endif

void ide_setup(void);
int ide_read(int drive, uint8_t *buffer, size_t size, size_t lba);
int ide_write(int drive, uint8_t *buffer, size_t size, size_t lba);
int ide_detect_device(uint8_t port, uint8_t device);

#ifdef __cplusplus
}
#endif
