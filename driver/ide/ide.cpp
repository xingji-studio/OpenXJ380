#include <ide/ide.h>
#include <proto.hpp>
#include <pci/pci.h>
#include <block_source.h>
#include <fs/vfs/vfs.h>
#include <ioctl.h>

static ide_device_t ide_devices[4]; // 最多 4 个 IDE 设备
extern vfs_node_t devfs_root;

static bool ide_wait_not_busy(uint16_t base, uint32_t timeout) {
    while (timeout-- > 0) {
        uint8_t status = inb(base + 7);
        if (status != 0xFF && (status & IDE_STATUS_BSY) == 0) {
            return true;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    write_serial_fmt("IDE: wait_not_busy timeout on base 0x%x status=0x%x\n", base, inb(base + 7));
    return false;
}

// 等待数据就绪
static int ide_wait_drq(uint16_t base) {
    int timeout = 100000;
    while (timeout--) {
        uint8_t status = inb(base + 7);
        if (status & IDE_STATUS_DRQ) return 0;
        if (status == 0 || status == 0xFF) return -1;
        if (status & (IDE_STATUS_DF | IDE_STATUS_ERR)) return -1;
        __asm__ __volatile__("pause" ::: "memory");
    }
    write_serial_fmt("IDE: wait_drq timeout on base 0x%x status=0x%x\n", base, inb(base + 7));
    return -1;
}

// Original bounded, read-only ATAPI PIO transport. All phases are polled;
// the channel IRQ stays disabled because this driver has no IRQ completion path.
static void ide_delay_400ns(uint16_t base)
{
    for (unsigned i = 0; i < 4; ++i) inb(base + 0x206);
}

static int ide_packet_status(uint16_t base, uint8_t *status)
{
    for (unsigned polls = 0; polls < 1000000; ++polls)
    {
        *status = inb(base + 7);
        if (*status == 0 || *status == 0xff) return -1;
        if (!(*status & IDE_STATUS_BSY))
            return *status & (IDE_STATUS_ERR | IDE_STATUS_DF) ? -1 : 0;
        __asm__ __volatile__("pause" ::: "memory");
    }
    return -1;
}

static int ide_packet_read(uint16_t base, uint8_t select, uint8_t packet_bytes,
                           const uint8_t command[16], uint8_t *buffer, size_t bytes)
{
    if (!command || !buffer || !bytes || bytes > 65534 || (packet_bytes != 12 && packet_bytes != 16)) return -1;
    if (!ide_wait_not_busy(base, 1000000)) return -1;
    outb(base + 0x206, 2); // nIEN, PIO only.
    outb(base + 6, select);
    ide_delay_400ns(base);
    outb(base + 1, 0); // Features: disable DMA.
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, (uint8_t)bytes);
    outb(base + 5, (uint8_t)(bytes >> 8));
    outb(base + 7, IDE_CMD_PACKET);
    ide_delay_400ns(base);
    uint8_t status;
    if (ide_packet_status(base, &status) || !(status & IDE_STATUS_DRQ) || (inb(base + 2) & 3) != 1) return -1;
    for (unsigned i = 0; i < packet_bytes; i += 2)
        outw(base, (uint16_t)command[i] | ((uint16_t)command[i + 1] << 8));
    ide_delay_400ns(base);
    size_t transferred = 0;
    // A phase can split a sector, but cannot make this an unbounded device-
    // controlled loop. Each accepted phase must contribute at least one byte.
    for (size_t phases = 0; phases <= bytes; ++phases)
    {
        if (ide_packet_status(base, &status)) return -1;
        if (!(status & IDE_STATUS_DRQ)) return transferred == bytes ? 0 : -1;
        if ((inb(base + 2) & 3) != 2) return -1; // Data-in, never data-out.
        size_t phase_bytes = (size_t)inb(base + 4) | ((size_t)inb(base + 5) << 8);
        if (!phase_bytes || phase_bytes > bytes - transferred) return -1;
        // String I/O transfers this already validated data-in phase without
        // a separate virtual-machine I/O exit for every 16-bit word.
        size_t whole_words = phase_bytes / 2;
        if (whole_words) insw(base, buffer + transferred, whole_words);
        if (phase_bytes & 1)
        {
            uint16_t word = inw(base);
            buffer[transferred + phase_bytes - 1] = (uint8_t)word;
        }
        transferred += phase_bytes;
        ide_delay_400ns(base);
    }
    return -1;
}

static uint32_t ide_packet_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static int ide_packet_capacity(uint16_t base, uint8_t select, uint8_t packet_bytes,
                               uint32_t *sectors, uint32_t *sector_size)
{
    uint8_t command[16] = {0x25}; // SCSI READ CAPACITY(10).
    uint8_t capacity[8] = {};
    int result = ide_packet_read(base, select, packet_bytes, command, capacity, sizeof(capacity));
    if (result)
    {
        // Clear an initial media-change/power-on Unit Attention, but do not
        // retry arbitrary I/O errors or register an empty optical drive.
        uint8_t sense_command[16] = {3, 0, 0, 0, 18};
        uint8_t sense[18] = {};
        if (ide_packet_read(base, select, packet_bytes, sense_command, sense, sizeof(sense)) ||
            (sense[0] & 0x7e) != 0x70 || (sense[2] & 15) != 6) return -1;
        result = ide_packet_read(base, select, packet_bytes, command, capacity, sizeof(capacity));
    }
    if (result) return -1;
    uint32_t last = ide_packet_be32(capacity);
    uint32_t block = ide_packet_be32(capacity + 4);
    if (last == 0xffffffffU || block < 512 || block > 65534 || (block & (block - 1))) return -1;
    *sectors = last + 1;
    *sector_size = block;
    return 0;
}

// 检测 IDE 设备
int ide_detect_device(uint8_t port, uint8_t device) {
    uint16_t base = (port == 0) ? IDE_PRIMARY_DATA_PORT : IDE_SECONDARY_DATA_PORT;
    uint8_t drive_select = (device == 0) ? IDE_DEVICE_MASTER : IDE_DEVICE_SLAVE;
    
    // 选择设备
    outb(base + 6, drive_select);
    
    // 等待
    for (int i = 0; i < 4; i++) inb(base + 7);
    
    // 检查是否存在
    uint8_t status = inb(base + 7);
    if (status == 0xFF) return -1;
    
    // 发送 IDENTIFY 命令
    outb(base + 6, drive_select);
    for (int i = 0; i < 4; i++) inb(base + 7);
    
    outb(base + 2, 0);  // 扇区计数
    outb(base + 3, 0);  // LBA 低
    outb(base + 4, 0);  // LBA 中
    outb(base + 5, 0);  // LBA 高
    outb(base + 7, IDE_CMD_IDENTIFY);
    
    // 读取状态
    status = inb(base + 7);
    if (status == 0) return -1;
    
    if (!ide_wait_not_busy(base, 1000000)) return -1;

    uint8_t lba_mid = inb(base + 4);
    uint8_t lba_hi  = inb(base + 5);
    bool packet = (lba_mid == 0x14 && lba_hi == 0xEB) || (lba_mid == 0x69 && lba_hi == 0x96);
    if (packet)
    {
        outb(base + 7, IDE_CMD_IDENTIFY_PACKET);
        ide_delay_400ns(base);
        if (!ide_wait_not_busy(base, 1000000)) return -1;
    }
    
    // 检查错误
    status = inb(base + 7);
    if (status == 0 || status == 0xFF || (status & IDE_STATUS_ERR)) return -1;
    
    // 等待 DRQ
    if (ide_wait_drq(base) != 0) return -1;
    
    // 读取 IDENTIFY 数据
    uint16_t data[256];
    for (int i = 0; i < 256; i++) {
        data[i] = inw(base);
    }
    
    // 解析设备信息
    int idx = port * 2 + device;
    ide_devices[idx].exists = 0;
    ide_devices[idx].is_master = (device == 0);
    ide_devices[idx].is_primary = (port == 0);
    ide_devices[idx].device_type = packet ? IDE_DEVICE_ATAPI : IDE_DEVICE_ATA;
    
    // 获取容量
    ide_devices[idx].sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
    ide_devices[idx].sector_size = 512;
    ide_devices[idx].packet_bytes = 0;
    if (packet)
    {
        // This path supports read-only CD/DVD media, not tapes or PMP targets.
        if (((data[0] >> 8) & 31) != 5 || (data[0] & 3) > 1) return -1;
        ide_devices[idx].packet_bytes = (data[0] & 3) ? 16 : 12;
        if (ide_packet_capacity(base, drive_select, ide_devices[idx].packet_bytes,
                                &ide_devices[idx].sectors, &ide_devices[idx].sector_size)) return -1;
    }
    if (!ide_devices[idx].sectors) return -1;
    ide_devices[idx].exists = 1;
    
    // 获取型号
    for (int i = 0; i < 40; i += 2) {
        ide_devices[idx].model[i] = (char)(data[27 + i/2] >> 8);
        ide_devices[idx].model[i+1] = (char)(data[27 + i/2] & 0xFF);
    }
    ide_devices[idx].model[40] = '\0';
    
    // 获取序列号
    for (int i = 0; i < 20; i += 2) {
        ide_devices[idx].serial[i] = (char)(data[10 + i/2] >> 8);
        ide_devices[idx].serial[i+1] = (char)(data[10 + i/2] & 0xFF);
    }
    ide_devices[idx].serial[20] = '\0';
    
    write_serial_fmt("IDE: Found device %d: %s, %d sectors\n", idx, ide_devices[idx].model, ide_devices[idx].sectors);
    
    return 0;
}

// IDE 读操作
int ide_read(int drive, uint8_t *buffer, size_t size, size_t lba) {
    if (drive < 0 || drive > 3 || !ide_devices[drive].exists) return -1;
    if (buffer == NULL && size != 0) return -1;
    if (lba > ide_devices[drive].sectors || size > ide_devices[drive].sectors - lba) return -1;

    uint16_t base = ide_devices[drive].is_primary ? IDE_PRIMARY_DATA_PORT : IDE_SECONDARY_DATA_PORT;
    uint8_t drive_select = ide_devices[drive].is_master ? IDE_DEVICE_MASTER : IDE_DEVICE_SLAVE;

    if (ide_devices[drive].device_type == IDE_DEVICE_ATAPI)
    {
        size_t block_bytes = ide_devices[drive].sector_size;
        if (!block_bytes || block_bytes > 65534) return -1;
        // ATAPI's byte-count register is 16-bit. Keep each READ(12) below
        // that limit and retain the per-phase status and exact-byte checks.
        size_t maximum_blocks = 65534 / block_bytes;
        for (size_t sector_index = 0; sector_index < size;)
        {
            size_t blocks = size - sector_index;
            if (blocks > maximum_blocks) blocks = maximum_blocks;
            uint64_t current_lba = lba + sector_index;
            uint8_t command[16] = {0xa8}; // SCSI READ(12), bounded optical blocks.
            command[2] = (uint8_t)(current_lba >> 24);
            command[3] = (uint8_t)(current_lba >> 16);
            command[4] = (uint8_t)(current_lba >> 8);
            command[5] = (uint8_t)current_lba;
            command[6] = (uint8_t)(blocks >> 24);
            command[7] = (uint8_t)(blocks >> 16);
            command[8] = (uint8_t)(blocks >> 8);
            command[9] = (uint8_t)blocks;
            if (ide_packet_read(base, drive_select, ide_devices[drive].packet_bytes, command,
                                buffer + sector_index * block_bytes, blocks * block_bytes)) return -1;
            sector_index += blocks;
        }
        return 0;
    }

    for (size_t sector_index = 0; sector_index < size; sector_index++) {
        uint64_t current_lba = lba + sector_index;
        if (current_lba > 0x0fffffffULL) return -1;

        // 等待就绪
        if (!ide_wait_not_busy(base, 1000000)) return -1;

        // 选择设备和 LBA 模式
        outb(base + 6, drive_select | 0x40 | ((current_lba >> 24) & 0x0F));

        // 设置扇区数和 LBA 地址
        outb(base + 2, 1);  // 读取 1 个扇区
        outb(base + 3, current_lba & 0xFF);
        outb(base + 4, (current_lba >> 8) & 0xFF);
        outb(base + 5, (current_lba >> 16) & 0xFF);

        // 发送读命令
        outb(base + 7, IDE_CMD_READ_SECTORS);

        // 等待数据就绪
        if (ide_wait_drq(base) != 0) return -1;

        // 读取数据
        uint16_t *buf = (uint16_t *)(buffer + sector_index * ide_devices[drive].sector_size);
        for (int i = 0; i < 256; i++) {
            buf[i] = inw(base);
        }
    }

    return 0;
}

// IDE 写操作
int ide_write(int drive, uint8_t *buffer, size_t size, size_t lba) {
    if (drive < 0 || drive > 3 || !ide_devices[drive].exists) return -1;
    if (ide_devices[drive].device_type == IDE_DEVICE_ATAPI) return -1;
    if (buffer == NULL && size != 0) return -1;
    if (lba > ide_devices[drive].sectors || size > ide_devices[drive].sectors - lba) return -1;

    uint16_t base = ide_devices[drive].is_primary ? IDE_PRIMARY_DATA_PORT : IDE_SECONDARY_DATA_PORT;
    uint8_t drive_select = ide_devices[drive].is_master ? IDE_DEVICE_MASTER : IDE_DEVICE_SLAVE;

    for (size_t sector_index = 0; sector_index < size; sector_index++) {
        uint64_t current_lba = lba + sector_index;

        // 等待就绪
        if (!ide_wait_not_busy(base, 1000000)) return -1;

        // 选择设备和 LBA 模式
        outb(base + 6, drive_select | 0x40 | ((current_lba >> 24) & 0x0F));

        // 设置扇区数和 LBA 地址
        outb(base + 2, 1);  // 写入 1 个扇区
        outb(base + 3, current_lba & 0xFF);
        outb(base + 4, (current_lba >> 8) & 0xFF);
        outb(base + 5, (current_lba >> 16) & 0xFF);

        // 发送写命令
        outb(base + 7, IDE_CMD_WRITE_SECTORS);

        // 等待设备请求数据
        if (ide_wait_drq(base) != 0) {
            write_serial_fmt("IDE: write lba=%llu did not enter DRQ\n", current_lba);
            return -1;
        }

        // 写入数据
        uint16_t *buf = (uint16_t *)(buffer + sector_index * ide_devices[drive].sector_size);
        for (int i = 0; i < 256; i++) {
            outw(base, buf[i]);
        }

        // 等待完成
        if (!ide_wait_not_busy(base, 1000000)) return -1;
    }

    return 0;
}

static int ide_flush(int drive) {
    if (drive < 0 || drive > 3 || !ide_devices[drive].exists) return -1;
    if (ide_devices[drive].device_type == IDE_DEVICE_ATAPI) return 0;

    uint16_t base = ide_devices[drive].is_primary ? IDE_PRIMARY_DATA_PORT : IDE_SECONDARY_DATA_PORT;
    uint8_t drive_select = ide_devices[drive].is_master ? IDE_DEVICE_MASTER : IDE_DEVICE_SLAVE;

    if (!ide_wait_not_busy(base, 1000000)) return -1;
    outb(base + 6, drive_select);
    for (int i = 0; i < 4; i++) inb(base + 7);
    outb(base + 7, IDE_CMD_CACHE_FLUSH);
    if (!ide_wait_not_busy(base, 1000000)) return -1;
    uint8_t status = inb(base + 7);
    return status == 0 || status == 0xff || (status & (IDE_STATUS_ERR | IDE_STATUS_DF)) ? -1 : 0;
}

// IDE 设备读取回调
static size_t ide_read_callback(int drive, uint8_t *buffer, size_t size, size_t lba) {
    int local_drive = -1;
    for (int i = 0; i < 4; i++) {
        if (ide_devices[i].exists && ide_devices[i].drive_id == drive) {
            local_drive = i;
            break;
        }
    }
    if (local_drive < 0) {
        write_serial_fmt("IDE: read callback cannot map global drive id %d\n", drive);
        return 0;
    }
    if (ide_read(local_drive, buffer, size, lba) != 0) return 0;
    return size;
}

// IDE 设备写入回调
static size_t ide_write_callback(int drive, uint8_t *buffer, size_t size, size_t lba) {
    int local_drive = -1;
    for (int i = 0; i < 4; i++) {
        if (ide_devices[i].exists && ide_devices[i].drive_id == drive) {
            local_drive = i;
            break;
        }
    }
    if (local_drive < 0) {
        write_serial_fmt("IDE: write callback cannot map global drive id %d\n", drive);
        return 0;
    }
    if (ide_write(local_drive, buffer, size, lba) != 0) return 0;
    return size;
}

static int ide_ioctl(device_t *device, size_t req, void *arg) {
    (void)arg;
    if (device == NULL) return -1;
    if (req != IOBLKSYNC) return 0;

    int local_drive = -1;
    for (int i = 0; i < 4; i++) {
        if (ide_devices[i].exists && ide_devices[i].drive_id == (int)device->vdiskid) {
            local_drive = i;
            break;
        }
    }
    if (local_drive < 0) {
        write_serial_fmt("IDE: ioctl cannot map global drive id %d\n", device->vdiskid);
        return -1;
    }
    return ide_flush(local_drive);
}

// 等待 devfs 就绪
static void wait_devfs_ready(void) {
    // 自旋等待 devfs_root 被设置
    uint32_t timeout = 1000000;
    while (devfs_root == NULL && timeout-- > 0) {
        // 可以添加 pause 指令减少功耗
        __asm__ __volatile__("pause" ::: "memory");
    }
    if (devfs_root == NULL) {
        write_serial_string("IDE: devfs_root not ready, skip IDE setup\n");
    }
}

// IDE 初始化
void ide_setup(void) {
    write_serial_string("Initializing IDE controller...\n");
    
    // 查找 IDE 控制器
    pci_device_t *ide_controller = pci_find_class(0x01018a);
    if (ide_controller == NULL) {
        write_serial_string("IDE: No IDE controller found\n");
        return;
    }
    
    write_serial_string("IDE: Found IDE controller\n");

    // Fixed 0x1f0/0x170 channels are attributable only to a unique enabled
    // compatibility-mode PCI controller, for both ATA disks and ATAPI optical media.
    unsigned source_controllers = 0;
    for (uint32_t i = 0; i < get_pci_num() && i < PCI_DEVICE_MAX; ++i)
    {
        pci_device_t *candidate = pci_devices[i];
        if (!candidate || (candidate->class_code >> 8) != 0x0101 ||
            (candidate->class_code & 5) || !candidate->op || !candidate->op->read) continue;
        if (candidate->op->read(candidate->bus, candidate->slot, candidate->func, candidate->segment, 4) & 1)
            ++source_controllers;
    }
    bool source_known = source_controllers == 1 && !(ide_controller->class_code & 5) &&
        ide_controller->op && ide_controller->op->read &&
        (ide_controller->op->read(ide_controller->bus, ide_controller->slot, ide_controller->func,
                                  ide_controller->segment, 4) & 1);
    
    // 初始化设备数组
    memset(ide_devices, 0, sizeof(ide_devices));
    
    // 初始化 drive_id 字段
    for (int i = 0; i < 4; i++) {
        ide_devices[i].drive_id = -1;
    }
    
    // 等待 devfs 就绪，而不是用延迟
    wait_devfs_ready();
    if (devfs_root == NULL) return;
    
    // 检测主通道主从设备
    write_serial_string("IDE: Detecting Primary Master...\n");
    if (ide_detect_device(0, 0) == 0) {
        write_serial_string("IDE: Primary Master detected\n");
    }
    
    write_serial_string("IDE: Detecting Primary Slave...\n");
    if (ide_detect_device(0, 1) == 0) {
        write_serial_string("IDE: Primary Slave detected\n");
    }
    
    // 检测从通道主从设备
    write_serial_string("IDE: Detecting Secondary Master...\n");
    if (ide_detect_device(1, 0) == 0) {
        write_serial_string("IDE: Secondary Master detected\n");
    }
    
    write_serial_string("IDE: Detecting Secondary Slave...\n");
    if (ide_detect_device(1, 1) == 0) {
        write_serial_string("IDE: Secondary Slave detected\n");
    }
    
    write_serial_string("IDE: Registering devices...\n");
    
    // 注册找到的设备
    for (int i = 0; i < 4; i++) {
        if (!ide_devices[i].exists) continue;
        
        char name_buf[20];
        sprintf(name_buf, "ide%d", i);
        
        device_t ide_dev = {};
        ide_dev.size = (uint64_t)ide_devices[i].sectors * ide_devices[i].sector_size;
        ide_dev.sector_size = ide_devices[i].sector_size;
        ide_dev.type = DEVICE_BLOCK;
        ide_dev.read = ide_read_callback;
        ide_dev.write = ide_devices[i].device_type == IDE_DEVICE_ATAPI ? NULL : ide_write_callback;
        ide_dev.flag = 1;
        ide_dev.ioctl = ide_ioctl;
        ide_dev.poll = (pollf)empty;
        ide_dev.map = (mapf)empty;
        ide_dev.read_vbuf = NULL;
        ide_dev.write_vbuf = NULL;
        ide_dev.max_size = __UINT64_MAX__;
        strcpy(ide_dev.drive_name, name_buf);
        ide_dev.path = NULL;  // 让 regist_device 分配路径
        
        write_serial_fmt("IDE: Registering %s with %d sectors\n", name_buf, ide_devices[i].sectors);
        
        int id = ide_dev.write ? regist_durable_block_device(NULL, ide_dev) : regist_device(NULL, ide_dev);
        if (id >= 0) {
            ide_devices[i].drive_id = id;  // 保存设备 ID
            if (source_known)
            {
                BlockSourceHardware provenance = {};
                provenance.controller = ide_controller;
                provenance.transport = XJ_INSTALLER_SOURCE_IDE;
                provenance.block_size = ide_dev.sector_size;
                provenance.bytes = ide_dev.size;
                provenance.read_only = ide_devices[i].device_type == IDE_DEVICE_ATAPI;
                provenance.ide_channel = (uint8_t)(i / 2);
                provenance.ide_unit = (uint8_t)(i % 2);
                int source_status = block_source_register((uint32_t)id, provenance);
                if (source_status) write_serial_fmt("IDE source provenance unavailable: %d\n", source_status);
            }
            write_serial_fmt("ide%d registered with id %d\n", i, id);
        }
        
        write_serial_fmt("ide%d: blk_size=%d, blk=0..%d, %s\n", 
                        i, ide_devices[i].sector_size, 
                        ide_devices[i].sectors, 
                        ide_devices[i].model);
    }
    
    write_serial_string("IDE controller initialized.\n");
}
