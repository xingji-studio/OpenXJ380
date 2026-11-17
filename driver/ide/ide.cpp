// SPDX-License-Identifier: Apache-2.0
// Original XJ380 PCI IDE/ATA PIO implementation. See docs/IDE_LICENSE_AUDIT.md.
#include <block_source.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <ide/ide.h>
#include <ioctl.h>
#include <mutex.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <task/scheduler.h>
#include <task/termination.h>
#include <wsod_storage.h>

struct IdeChannel
{
    uint16_t      command;
    uint16_t      control;
    pci_device_t *controller;
    mutex_t       mutex;
    uint8_t       number;
    bool          present;
    bool          native;
    bool          failed;
    bool          active;
};

static IdeChannel   ide_channels[IDE_MAX_CHANNELS];
static ide_device_t ide_devices[IDE_MAX_DEVICES];
static unsigned     ide_channel_count;
extern vfs_node_t   devfs_root;

// The clock is usable before the scheduler starts. A stopped/unavailable clock
// must still terminate a poll, including the independent fatal-error path.
class IdeDeadline {
    uint64_t start   = nanoTime();
    uint64_t last    = start;
    unsigned stalled = 0;
    uint64_t timeout;

  public:
    explicit IdeDeadline(uint64_t nanoseconds = 5000000000ULL) : timeout(nanoseconds) {}

    bool expired()
    {
        uint64_t now = nanoTime();
        if (now - start >= timeout) return true;
        if (now != last)
        {
            last    = now;
            stalled = 0;
            return false;
        }
        return ++stalled >= 1000000;
    }
};

class IdeOperation {
    // Keep an owner/waiter alive until both the PIO transaction and mutex unwind.
    KernelOperationScope operation;
    IdeChannel          *channel;
    bool                 locked  = false;
    bool                 claimed = false;

  public:
    explicit IdeOperation(IdeChannel *value) : channel(value)
    {
        if (!channel || !channel->present) return;
        if (get_current_task() && scheduler_global_enabled())
        {
            if (!scheduler_is_enabled() || !are_interrupts_enabled()) return;
            locked = mutex_lock(&channel->mutex) == 0;
            if (!locked) return;
        }
        // Discovery before scheduler_start is synchronous. The atomic claim
        // also prevents fatal output from overwriting an interrupted command.
        bool expected = false;
        claimed =
            __atomic_compare_exchange_n(&channel->active, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    ~IdeOperation()
    {
        if (claimed) __atomic_store_n(&channel->active, false, __ATOMIC_RELEASE);
        if (locked) mutex_unlock(&channel->mutex);
    }

    bool ready() const
    {
        return claimed && !__atomic_load_n(&channel->failed, __ATOMIC_ACQUIRE);
    }
};

static void ide_delay_400ns(const IdeChannel &channel)
{
    // Alternate Status never acknowledges an interrupt. Native control BARs
    // are independent of the command BAR, unlike the legacy +206h layout.
    for (unsigned i = 0; i < 4; ++i)
        inb(channel.control);
}

static int ide_wait(const IdeChannel &channel, IdeDeadline &deadline, bool data, bool check_error = true)
{
    do
    {
        uint8_t status = inb(channel.control);
        if (status == 0 || status == 0xff) return -ENODEV;
        // DRQ and error bits are not valid while BSY is asserted.
        if (!(status & IDE_STATUS_BSY))
        {
            if (check_error && (status & (IDE_STATUS_ERR | IDE_STATUS_DF))) return -EIO;
            if (bool(status & IDE_STATUS_DRQ) == data) return 0;
        }
        asm volatile("pause" ::: "memory");
    } while (!deadline.expired());
    return -ETIMEDOUT;
}

static int ide_select(const IdeChannel &channel, uint8_t select, IdeDeadline &deadline)
{
    // Only device IRQs are masked; CPU interrupts/preemption remain available.
    outb(channel.control, 2);
    // An absent previous selection is legal when probing a slave-only channel.
    uint8_t status = inb(channel.control);
    if (status != 0 && status != 0xff)
    {
        int error = ide_wait(channel, deadline, false, false);
        if (error) return error;
    }
    outb(channel.command + 6, select);
    ide_delay_400ns(channel);
    return 0;
}

static int ide_io_failed(IdeChannel &channel, int error)
{
    if (!error) return 0;
    // Never replay an uncertain write or let a new command consume residual
    // DRQ data. Recovery requires reboot/discovery, not a stolen runtime lock.
    __atomic_store_n(&channel.failed, true, __ATOMIC_RELEASE);
    try_write_serial_fmt("IDE: channel %u failed error=%d status=0x%x; I/O quarantined\n", channel.number, error,
                         inb(channel.control));
    return error;
}

static int ide_packet_status(const IdeChannel &channel, IdeDeadline &deadline, uint8_t *status)
{
    do
    {
        *status = inb(channel.control);
        if (*status == 0 || *status == 0xff) return -ENODEV;
        if (!(*status & IDE_STATUS_BSY)) return *status & (IDE_STATUS_ERR | IDE_STATUS_DF) ? -EIO : 0;
        asm volatile("pause" ::: "memory");
    } while (!deadline.expired());
    return -ETIMEDOUT;
}

static int ide_packet_read(const IdeChannel &channel, uint8_t select, uint8_t packet_bytes, const uint8_t command[16],
                           uint8_t *buffer, size_t bytes)
{
    if (!command || !buffer || !bytes || bytes > 65534 || (packet_bytes != 12 && packet_bytes != 16)) return -EINVAL;
    IdeDeadline deadline;
    int         error = ide_select(channel, select, deadline);
    if (error) return error;
    uint16_t base = channel.command;
    outb(base + 1, 0); // Features: PIO, no DMA.
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, uint8_t(bytes));
    outb(base + 5, uint8_t(bytes >> 8));
    outb(base + 7, IDE_CMD_PACKET);
    ide_delay_400ns(channel);
    uint8_t status;
    error = ide_packet_status(channel, deadline, &status);
    if (error) return error;
    if (!(status & IDE_STATUS_DRQ) || (inb(base + 2) & 3) != 1) return -EIO;
    for (unsigned i = 0; i < packet_bytes; i += 2)
        outw(base, uint16_t(command[i]) | (uint16_t(command[i + 1]) << 8));
    ide_delay_400ns(channel);
    size_t transferred = 0;
    // Bound both the phase count and exact data-in byte count. A phase may
    // split a sector and may have an odd number of useful bytes.
    for (size_t phases = 0; phases <= bytes; ++phases)
    {
        error = ide_packet_status(channel, deadline, &status);
        if (error) return error;
        if (!(status & IDE_STATUS_DRQ)) return transferred == bytes ? 0 : -EIO;
        if ((inb(base + 2) & 3) != 2) return -EIO;
        size_t phase_bytes = size_t(inb(base + 4)) | (size_t(inb(base + 5)) << 8);
        if (!phase_bytes || phase_bytes > bytes - transferred) return -EIO;
        size_t words = phase_bytes / 2;
        if (words) insw(base, buffer + transferred, words);
        if (phase_bytes & 1) buffer[transferred + phase_bytes - 1] = uint8_t(inw(base));
        transferred += phase_bytes;
        ide_delay_400ns(channel);
    }
    return -EIO;
}

static uint32_t ide_packet_be32(const uint8_t *bytes)
{
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
}

static int ide_packet_capacity(const IdeChannel &channel, uint8_t select, ide_device_t &disk)
{
    uint8_t command[16] = {0x25}; // SCSI READ CAPACITY(10).
    uint8_t capacity[8] = {};
    int     result      = ide_packet_read(channel, select, disk.packet_bytes, command, capacity, sizeof(capacity));
    if (result)
    {
        // Clear one initial power-on/media-change Unit Attention only.
        uint8_t sense_command[16] = {3, 0, 0, 0, 18};
        uint8_t sense[18]         = {};
        if (ide_packet_read(channel, select, disk.packet_bytes, sense_command, sense, sizeof(sense)) ||
            (sense[0] & 0x7e) != 0x70 || (sense[2] & 15) != 6)
            return -EIO;
        result = ide_packet_read(channel, select, disk.packet_bytes, command, capacity, sizeof(capacity));
    }
    if (result) return result;
    uint32_t last  = ide_packet_be32(capacity);
    uint32_t block = ide_packet_be32(capacity + 4);
    if (last == 0xffffffffU || block < 512 || block > 65534 || (block & (block - 1))) return -EOPNOTSUPP;
    disk.sectors     = uint64_t(last) + 1;
    disk.sector_size = block;
    return 0;
}

static bool ide_identify_ata(const uint16_t data[256], ide_device_t &disk)
{
    if (!(data[49] & (1U << 9))) return false; // CHS-only disks are unsupported.
    bool valid_commands = (data[83] & 0xc000) == 0x4000;
    disk.lba48          = valid_commands && (data[83] & (1U << 10));
    disk.sectors        = uint32_t(data[60]) | (uint32_t(data[61]) << 16);
    if (disk.lba48)
    {
        disk.sectors = uint64_t(data[100]) | (uint64_t(data[101]) << 16) | (uint64_t(data[102]) << 32) |
                       (uint64_t(data[103]) << 48);
        if (!disk.sectors || disk.sectors > (1ULL << 48)) return false;
    }
    else if (!disk.sectors || disk.sectors > (1ULL << 28))
        return false;
    // IDENTIFY 106 describes *logical* sectors. 512e still transfers 256 words;
    // never expose a 4Kn disk with a fabricated 512-byte logical sector size.
    if ((data[106] & 0xc000) == 0x4000 && (data[106] & (1U << 12)) &&
        (uint32_t(data[117]) | (uint32_t(data[118]) << 16)) != 256)
        return false;
    disk.sector_size   = 512;
    disk.flush_command = valid_commands && (data[83] & (1U << 12)) ? IDE_CMD_CACHE_FLUSH : 0;
    if (disk.lba48 && valid_commands && (data[83] & (1U << 13))) disk.flush_command = IDE_CMD_CACHE_FLUSH_EXT;
    return true;
}

int ide_detect_device(uint8_t port, uint8_t device)
{
    if (port >= ide_channel_count || device > 1) return -EINVAL;
    IdeChannel  &channel = ide_channels[port];
    IdeOperation operation(&channel);
    if (!operation.ready()) return -EIO;
    unsigned index            = unsigned(port) * 2 + device;
    ide_devices[index].exists = 0;
    IdeDeadline deadline;
    uint8_t     select = device ? IDE_DEVICE_SLAVE : IDE_DEVICE_MASTER;
    int         error  = ide_select(channel, select, deadline);
    if (error) return error;
    uint16_t base   = channel.command;
    uint8_t  status = inb(channel.control);
    if (!status || status == 0xff) return -ENODEV;
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 0);
    outb(base + 7, IDE_CMD_IDENTIFY);
    ide_delay_400ns(channel);
    // IDENTIFY on ATAPI fails with a signature, not an ATA data phase.
    do
    {
        status = inb(channel.control);
        if (!status || status == 0xff) return -ENODEV;
        if (!(status & IDE_STATUS_BSY)) break;
        if (deadline.expired()) return -ETIMEDOUT;
        asm volatile("pause" ::: "memory");
    } while (true);
    uint8_t mid = inb(base + 4), high = inb(base + 5);
    bool    packet = (mid == 0x14 && high == 0xeb) || (mid == 0x69 && high == 0x96);
    if (packet)
    {
        outb(base + 7, IDE_CMD_IDENTIFY_PACKET);
        ide_delay_400ns(channel);
    }
    error = ide_wait(channel, deadline, true);
    if (error) return error;
    uint16_t data[256];
    insw(base, data, 256);
    ide_delay_400ns(channel);
    error = ide_wait(channel, deadline, false);
    if (error) return error;
    ide_device_t disk = {};
    disk.drive_id     = -1;
    disk.channel      = port;
    disk.is_master    = device == 0;
    disk.is_primary   = channel.number == 0;
    disk.device_type  = packet ? IDE_DEVICE_ATAPI : IDE_DEVICE_ATA;
    if (packet)
    {
        if (((data[0] >> 8) & 31) != 5 || (data[0] & 3) > 1) return -EOPNOTSUPP;
        disk.packet_bytes = (data[0] & 3) ? 16 : 12;
        error             = ide_packet_capacity(channel, select, disk);
        if (error) return error;
    }
    else if (!ide_identify_ata(data, disk))
        return -EOPNOTSUPP;
    for (unsigned i = 0; i < 40; i += 2)
    {
        disk.model[i]     = char(data[27 + i / 2] >> 8);
        disk.model[i + 1] = char(data[27 + i / 2]);
    }
    for (unsigned i = 0; i < 20; i += 2)
    {
        disk.serial[i]     = char(data[10 + i / 2] >> 8);
        disk.serial[i + 1] = char(data[10 + i / 2]);
    }
    for (unsigned i = 40; i && disk.model[i - 1] == ' '; --i)
        disk.model[i - 1] = 0;
    disk.exists        = 1;
    ide_devices[index] = disk;
    return 0;
}

static bool ide_range_valid(const ide_device_t &disk, const void *buffer, size_t size, uint64_t lba)
{
    uint64_t limit = disk.device_type == IDE_DEVICE_ATAPI ? disk.sectors : disk.lba48 ? (1ULL << 48) : (1ULL << 28);
    // Validate the complete command before any register write, including a
    // request which begins below 2^28 but ends above it.
    return disk.sector_size && (buffer || !size) && lba <= disk.sectors && size <= disk.sectors - lba && lba <= limit &&
           size <= limit - lba && size <= __SIZE_MAX__ / disk.sector_size;
}

static int ide_ata_transfer(const IdeChannel &channel, const ide_device_t &disk, uint8_t *buffer, size_t sectors,
                            uint64_t lba, bool write)
{
    for (size_t done = 0; done < sectors;)
    {
        // Bound a single command; the device-manager normally submits 8 sectors.
        unsigned    count    = sectors - done > 128 ? 128 : unsigned(sectors - done);
        uint64_t    at       = lba + done;
        bool        extended = at >= (1ULL << 28) || count > (1ULL << 28) - at;
        IdeDeadline deadline;
        uint8_t     select = (disk.is_master ? IDE_DEVICE_MASTER : IDE_DEVICE_SLAVE) | 0x40;
        if (!extended) select |= uint8_t((at >> 24) & 15);
        int error = ide_select(channel, select, deadline);
        if (error) return error;
        error = ide_wait(channel, deadline, false, false);
        if (error) return error;
        uint16_t base = channel.command;
        if (extended)
        {
            // ATA48 latches the high bank first, then the low bank. The Device
            // register's LBA28 head nibble must be zero for extended commands.
            outb(base + 1, 0);
            outb(base + 2, uint8_t(count >> 8));
            outb(base + 3, uint8_t(at >> 24));
            outb(base + 4, uint8_t(at >> 32));
            outb(base + 5, uint8_t(at >> 40));
        }
        outb(base + 1, 0);
        outb(base + 2, uint8_t(count));
        outb(base + 3, uint8_t(at));
        outb(base + 4, uint8_t(at >> 8));
        outb(base + 5, uint8_t(at >> 16));
        outb(base + 7, extended ? (write ? IDE_CMD_WRITE_SECTORS_EXT : IDE_CMD_READ_SECTORS_EXT)
                                : (write ? IDE_CMD_WRITE_SECTORS : IDE_CMD_READ_SECTORS));
        ide_delay_400ns(channel);
        for (unsigned i = 0; i < count; ++i)
        {
            error = ide_wait(channel, deadline, true);
            if (error) return error;
            uint8_t *data = buffer + (done + i) * 512;
            if (write)
                outsw(base, data, 256);
            else
                insw(base, data, 256);
            ide_delay_400ns(channel);
        }
        // A final-sector error is just as real as a failed DRQ phase.
        error = ide_wait(channel, deadline, false);
        if (error) return error;
        done += count;
    }
    return 0;
}

int ide_read(int drive, uint8_t *buffer, size_t size, size_t lba)
{
    if (drive < 0 || drive >= IDE_MAX_DEVICES || !ide_devices[drive].exists) return -ENODEV;
    ide_device_t &disk = ide_devices[drive];
    if (!ide_range_valid(disk, buffer, size, lba)) return -EINVAL;
    if (!size) return 0;
    IdeChannel  &channel = ide_channels[disk.channel];
    IdeOperation operation(&channel);
    if (!operation.ready()) return -EIO;
    int error = 0;
    if (disk.device_type == IDE_DEVICE_ATAPI)
    {
        size_t maximum = 65534 / disk.sector_size;
        for (size_t done = 0; done < size;)
        {
            size_t   count       = size - done > maximum ? maximum : size - done;
            uint64_t at          = lba + done;
            uint8_t  command[16] = {0xa8}; // SCSI READ(12).
            command[2]           = uint8_t(at >> 24);
            command[3]           = uint8_t(at >> 16);
            command[4]           = uint8_t(at >> 8);
            command[5]           = uint8_t(at);
            command[6]           = uint8_t(count >> 24);
            command[7]           = uint8_t(count >> 16);
            command[8]           = uint8_t(count >> 8);
            command[9]           = uint8_t(count);
            error = ide_packet_read(channel, disk.is_master ? IDE_DEVICE_MASTER : IDE_DEVICE_SLAVE, disk.packet_bytes,
                                    command, buffer + done * disk.sector_size, count * disk.sector_size);
            if (error) break;
            done += count;
        }
    }
    else
        error = ide_ata_transfer(channel, disk, buffer, size, lba, false);
    return ide_io_failed(channel, error);
}

int ide_write(int drive, uint8_t *buffer, size_t size, size_t lba)
{
    if (drive < 0 || drive >= IDE_MAX_DEVICES || !ide_devices[drive].exists) return -ENODEV;
    ide_device_t &disk = ide_devices[drive];
    if (disk.device_type == IDE_DEVICE_ATAPI) return -EROFS;
    if (!ide_range_valid(disk, buffer, size, lba)) return -EINVAL;
    if (!size) return 0;
    IdeChannel  &channel = ide_channels[disk.channel];
    IdeOperation operation(&channel);
    if (!operation.ready()) return -EIO;
    return ide_io_failed(channel, ide_ata_transfer(channel, disk, buffer, size, lba, true));
}

static int ide_flush_raw(const IdeChannel &channel, const ide_device_t &disk)
{
    if (!disk.flush_command) return -EOPNOTSUPP;
    IdeDeadline deadline(30000000000ULL);
    int         error = ide_select(channel, disk.is_master ? IDE_DEVICE_MASTER : IDE_DEVICE_SLAVE, deadline);
    if (error) return error;
    error = ide_wait(channel, deadline, false, false);
    if (error) return error;
    outb(channel.command + 7, disk.flush_command);
    ide_delay_400ns(channel);
    return ide_wait(channel, deadline, false);
}

static int ide_flush(int drive)
{
    if (drive < 0 || drive >= IDE_MAX_DEVICES || !ide_devices[drive].exists) return -ENODEV;
    ide_device_t &disk = ide_devices[drive];
    if (disk.device_type == IDE_DEVICE_ATAPI) return 0;
    if (!disk.flush_command) return -EOPNOTSUPP;
    IdeChannel  &channel = ide_channels[disk.channel];
    IdeOperation operation(&channel);
    if (!operation.ready()) return -EIO;
    return ide_io_failed(channel, ide_flush_raw(channel, disk));
}

static int ide_local_drive(int drive)
{
    for (int i = 0; i < IDE_MAX_DEVICES; ++i)
        if (ide_devices[i].exists && ide_devices[i].drive_id == drive) return i;
    return -1;
}

static size_t ide_read_callback(int drive, uint8_t *buffer, size_t size, size_t lba)
{
    return ide_read(ide_local_drive(drive), buffer, size, lba) == 0 ? size : 0;
}

static size_t ide_write_callback(int drive, uint8_t *buffer, size_t size, size_t lba)
{
    return ide_write(ide_local_drive(drive), buffer, size, lba) == 0 ? size : 0;
}

static int ide_ioctl(device_t *device, size_t request, void *argument)
{
    (void)argument;
    if (!device) return -ENODEV;
    if (request != IOBLKSYNC) return -EOPNOTSUPP;
    return ide_flush(ide_local_drive(int(device->vdiskid)));
}

static bool ide_decode_channel(pci_device_t *controller, unsigned number, IdeChannel &channel)
{
    channel.controller = controller;
    channel.number     = uint8_t(number);
    channel.native     = controller->class_code & (1U << (number * 2));
    if (channel.native)
    {
        auto     read = controller->op->read;
        uint32_t command =
            read(controller->bus, controller->slot, controller->func, controller->segment, 0x10 + number * 8);
        uint32_t control =
            read(controller->bus, controller->slot, controller->func, controller->segment, 0x14 + number * 8);
        if (!(command & 1) || !(control & 1)) return false;
        command &= ~3U;
        control &= ~3U;
        if (!command || !control || command > 0xfff8 || control > 0xfffc || (command & 7)) return false;
        channel.command = uint16_t(command);
        channel.control = uint16_t(control + 2);
    }
    else
    {
        channel.command = number ? IDE_SECONDARY_DATA_PORT : IDE_PRIMARY_DATA_PORT;
        channel.control = number ? 0x376 : 0x3f6;
    }
    return channel.control < channel.command || channel.control >= channel.command + 8;
}

static bool ide_channels_overlap(const IdeChannel &a, const IdeChannel &b)
{
    return (a.command < b.command + 8 && b.command < a.command + 8) || a.control == b.control ||
           (a.control >= b.command && a.control < b.command + 8) ||
           (b.control >= a.command && b.control < a.command + 8);
}

void ide_setup(void)
{
    write_serial_string("Initializing IDE controllers...\n");
    // Setup is a one-time boot operation: never reset live mutexes or publish
    // another alias for an already registered disk.
    if (ide_channel_count || !devfs_root) return;
    for (auto &disk : ide_devices)
        disk.drive_id = -1;
    for (uint32_t i = 0; i < get_pci_num() && i < PCI_DEVICE_MAX; ++i)
    {
        pci_device_t *controller = pci_devices[i];
        if (!controller || (controller->class_code >> 8) != 0x0101 || controller->header_type != 0 || !controller->op ||
            !controller->op->read)
            continue;
        // Firmware owns channel mode/timing configuration. Do not force a
        // native controller into compatibility mode or enable DMA bus mastering.
        uint32_t command = controller->op->read(controller->bus, controller->slot, controller->func,
                                                controller->segment, PCI_CONF_COMMAND);
        if (!(command & PCI_COMMAND_IO)) continue;
        if (ide_channel_count + 2 > IDE_MAX_CHANNELS)
        {
            write_serial_string("IDE: too many controllers; refusing ambiguous discovery\n");
            ide_channel_count = 0;
            return;
        }
        for (unsigned number = 0; number < 2; ++number)
        {
            IdeChannel &channel = ide_channels[ide_channel_count++];
            channel.present     = ide_decode_channel(controller, number, channel);
            mutex_create(&channel.mutex, false);
        }
    }
    // Resolve ownership before touching any task file. Mark every conflicting
    // channel, including three-way aliases, rather than attributing it by scan order.
    bool conflicts[IDE_MAX_CHANNELS] = {};
    for (unsigned i = 0; i < ide_channel_count; ++i)
        for (unsigned j = i + 1; j < ide_channel_count; ++j)
            if (ide_channels[i].present && ide_channels[j].present &&
                ide_channels_overlap(ide_channels[i], ide_channels[j]))
                conflicts[i] = conflicts[j] = true;
    for (unsigned port = 0; port < ide_channel_count; ++port)
    {
        IdeChannel &channel = ide_channels[port];
        if (conflicts[port])
        {
            channel.present = false;
            write_serial_fmt("IDE: conflicting I/O ownership on channel %u; skipped\n", port);
        }
        if (!channel.present) continue;
        pci_device_t *controller = channel.controller;
        write_serial_fmt("IDE: PCI %x:%x:%x.%x class=%x channel=%u %s cmd=0x%x ctl=0x%x\n", controller->segment,
                         controller->bus, controller->slot, controller->func, controller->class_code, channel.number,
                         channel.native ? "native" : "compatibility", channel.command, channel.control);
        for (unsigned unit = 0; unit < 2; ++unit)
        {
            int error = ide_detect_device(uint8_t(port), uint8_t(unit));
            if (error)
            {
                if (error != -ENODEV) write_serial_fmt("IDE: channel=%u unit=%u probe failed: %d\n", port, unit, error);
                // An incomplete probe cannot be followed by another task-file
                // command. A clean ATAPI no-medium/unsupported disk can be skipped.
                uint8_t status = inb(channel.control);
                if (error == -ETIMEDOUT || (status != 0xff && (status & (IDE_STATUS_BSY | IDE_STATUS_DRQ))))
                {
                    channel.failed = true;
                    break;
                }
            }
        }
        if (channel.failed) continue;
        for (unsigned unit = 0; unit < 2; ++unit)
        {
            unsigned      index = port * 2 + unit;
            ide_device_t &disk  = ide_devices[index];
            if (!disk.exists) continue;
            device_t device    = {};
            device.size        = disk.sectors * disk.sector_size;
            device.sector_size = disk.sector_size;
            device.type        = DEVICE_BLOCK;
            device.read        = ide_read_callback;
            device.write       = disk.device_type == IDE_DEVICE_ATAPI ? NULL : ide_write_callback;
            device.flag        = 1;
            device.ioctl       = ide_ioctl;
            device.poll        = (pollf)empty;
            device.map         = (mapf)empty;
            device.max_size    = __UINT64_MAX__;
            snprintf(device.drive_name, sizeof(device.drive_name), "ide%u", index);
            int id = device.write && disk.flush_command ? regist_durable_block_device(NULL, device)
                                                        : regist_device(NULL, device);
            if (id < 0) continue;
            disk.drive_id              = id;
            BlockSourceHardware source = {};
            source.controller          = controller;
            source.transport           = XJ_INSTALLER_SOURCE_IDE;
            source.block_size          = device.sector_size;
            source.bytes               = device.size;
            source.read_only           = !device.write;
            source.ide_channel         = channel.number;
            source.ide_unit            = uint8_t(unit);
            int source_error           = block_source_register(uint32_t(id), source);
            if (source_error) write_serial_fmt("IDE: source registration failed: %d\n", source_error);
            write_serial_fmt("IDE: %s registered id=%d sectors=%llu block=%u lba48=%u flush=%x model=%s\n",
                             device.drive_name, id, disk.sectors, disk.sector_size, disk.lba48, disk.flush_command,
                             disk.model);
        }
    }
    write_serial_string("IDE controllers initialized.\n");
}

int ide_wsod_prepare(const BlockExtent &extent, WsodDiskTarget *target)
{
    device_t *device = get_device(extent.physical_device);
    if (!target || !device || device->write != ide_write_callback) return -EOPNOTSUPP;
    int local = ide_local_drive(int(device->vdiskid));
    if (local < 0) return -EOPNOTSUPP;
    ide_device_t &disk    = ide_devices[local];
    IdeChannel   &channel = ide_channels[disk.channel];
    if (disk.device_type != IDE_DEVICE_ATA || disk.sector_size != 512 || !disk.flush_command ||
        __atomic_load_n(&channel.failed, __ATOMIC_ACQUIRE))
        return -EOPNOTSUPP;
    target->transport = 3;
    target->disk      = &disk;
    return 0;
}

// Fatal output never takes a sleeping mutex, allocates, retries a write, or
// steals a channel from an interrupted normal transaction on either unit.
static bool ide_wsod_claim(IdeChannel &channel)
{
    if (!channel.present || __atomic_load_n(&channel.failed, __ATOMIC_ACQUIRE)) return false;
    bool expected = false;
    return __atomic_compare_exchange_n(&channel.active, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

int ide_wsod_write(WsodDiskTarget *target, uint64_t lba, const void *data, unsigned sectors)
{
    auto *disk = target ? static_cast<ide_device_t *>(target->disk) : nullptr;
    if (!disk || !sectors || sectors > 8 || !ide_range_valid(*disk, data, sectors, lba)) return -EINVAL;
    IdeChannel &channel = ide_channels[disk->channel];
    if (!ide_wsod_claim(channel)) return -EBUSY;
    int error = ide_ata_transfer(channel, *disk, (uint8_t *)data, sectors, lba, true);
    if (error) __atomic_store_n(&channel.failed, true, __ATOMIC_RELEASE);
    __atomic_store_n(&channel.active, false, __ATOMIC_RELEASE);
    return error;
}

int ide_wsod_flush(WsodDiskTarget *target)
{
    auto *disk = target ? static_cast<ide_device_t *>(target->disk) : nullptr;
    if (!disk) return -EINVAL;
    IdeChannel &channel = ide_channels[disk->channel];
    if (!ide_wsod_claim(channel)) return -EBUSY;
    int error = ide_flush_raw(channel, *disk);
    if (error) __atomic_store_n(&channel.failed, true, __ATOMIC_RELEASE);
    __atomic_store_n(&channel.active, false, __ATOMIC_RELEASE);
    return error;
}
