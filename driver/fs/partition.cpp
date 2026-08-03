#include "fs/partition.h"
#include "device.h"
#include "errno.h"
#include "fs/vfs/vfs.h"
#include "krlibc.h"
#include <ioctl.h>
#include <dlinker.h>
extern device_t device_ctl[256];
partition_t     partitions[MAX_PARTITIONS_NUM];
size_t          partition_num = 0;
partition_t    *device_lists[MAX_PARTITIONS_NUM];
static bool     partition_runtime_ready = false;

size_t partition_read(int part, uint8_t *buf, size_t number, size_t lba);
size_t partition_write(int part, uint8_t *buf, size_t number, size_t lba);
bool   parser_block_device(vfs_node_t device, device_t disk, size_t vdisk_id);
int    partition_ioctl(device_t *device, size_t req, void *arg);

static bool partition_name_is_slice(const char *name)
{
    if (name == NULL) return false;
    size_t len = strlen(name);
    if (len < 3) return false;

    size_t pos = len;
    while (pos > 0 && isdigit((unsigned char)name[pos - 1]))
    {
        pos--;
    }
    if (pos == len || pos == 0 || name[pos - 1] != 'p') return false;
    return pos > 1;
}

static void partition_register_device_node(size_t partition_index)
{
    partition_t partition = partitions[partition_index];
    device_t    part;
    memset(&part, 0, sizeof(part));
    part.sector_size  = partition.sector_size;
    part.flag         = 1;
    part.type         = DEVICE_BLOCK;
    part.read         = partition_read;
    part.write        = partition_write;
    part.ioctl        = partition_ioctl;
    part.poll         = (pollf)empty;
    part.map          = (mapf)empty;
    part.read_vbuf    = NULL;
    part.write_vbuf   = NULL;
    part.size         = (partition.ending_lba - partition.starting_lba + 1) * partition.sector_size;
    device_t *src_dev = get_device(partition.vdisk_id);
    part.max_size     = src_dev ? src_dev->max_size : __UINT64_MAX__;
    if (src_dev == NULL) return;

    char buf[20];
    sprintf(buf, "%sp%zu", src_dev->drive_name, partition_index);
    strcpy(part.drive_name, buf);
    int id           = regist_device(NULL, part);
    if (id >= 0 && id < MAX_PARTITIONS_NUM) device_lists[id] = &partitions[partition_index];
}

static void partition_try_automount(size_t partition_index)
{
    partition_t partition = partitions[partition_index];
    device_t   *src_dev   = get_device(partition.vdisk_id);
    if (src_dev == NULL) return;

    char dev_path[64];
    char mount_path[64];
    sprintf(dev_path, "/dev/%sp%zu", src_dev->drive_name, partition_index);
    sprintf(mount_path, "/mnt/%sp%zu", src_dev->drive_name, partition_index);

    vfs_mkdir("/mnt");
    vfs_mkdir(mount_path);

    vfs_node_t mount_node = vfs_open(mount_path);
    if (mount_node == NULL || mount_node->is_mount) return;

    if (vfs_mount(dev_path, mount_node) == EOK)
    {
        pr_info("part: automount %s -> %s success\n", dev_path, mount_path);
    }
}

static void partition_scan_single_device(size_t vdisk_id, bool auto_mount)
{
    if (vdisk_id >= 256) return;
    device_t *disk = get_device(vdisk_id);
    if (disk == NULL || disk->type != DEVICE_BLOCK || disk->flag < 1) return;
    if (partition_name_is_slice(disk->drive_name)) return;

    char buf[50];
    sprintf(buf, "/dev/%s", disk->drive_name);
    pr_info("PART: %s\n", buf);
    vfs_node_t device = vfs_open(buf);
    if (device == NULL)
    {
        pr_info("part: Partition scan failed, device %s not found.\n", disk->drive_name);
        return;
    }

    size_t first_partition = partition_num;
    if (!parser_block_device(device, *disk, vdisk_id)) return;

    for (size_t j = first_partition; j < partition_num; j++)
    {
        partition_register_device_node(j);
        if (auto_mount) partition_try_automount(j);
    }
}

void partition_rescan_device(size_t vdisk_id, bool auto_mount)
{
    if (vdisk_id >= 256) return;
    partition_scan_single_device(vdisk_id, auto_mount);
}

int partition_find_first_for_disk(size_t vdisk_id)
{
    for (size_t i = partition_num; i > 0; i--)
    {
        if (partitions[i - 1].vdisk_id == vdisk_id) return (int)(i - 1);
    }
    return -1;
}

static void partition_dump_prefix(const char *name, const uint8_t *buf, size_t bytes)
{
    size_t dump_len = bytes > 64 ? 64 : bytes;
    pr_debug("PART: first %zu bytes of %s:", dump_len, name);
    for (size_t i = 0; i < dump_len; ++i)
    {
        if ((i % 16) == 0) write_serial_string("\nPART:   ");
        pr_info("%02x ", buf[i]);
    }
    write_serial_string("\n");
}

static bool partition_read_exact(vfs_node_t device, device_t disk, void *buf, size_t offset, size_t bytes,
                                 const char *tag)
{
    size_t got      = vfs_read(device, buf, offset, bytes);
    if (got != bytes)
    {
        pr_info("PART: %s short read offset=0x%zx bytes=0x%zx got=0x%zx sector_size=%zu\n", tag, offset,
                         bytes, got, disk.sector_size);
        return false;
    }
    return true;
}

size_t partition_read(int part, uint8_t *buf, size_t number, size_t lba)
{
    partition_t *partition = device_lists[part];
    // pr_info("Y K %s %d\n",device_ctl[partition->vdisk_id].drive_name,partition->vdisk_id);
    return device_ctl[partition->vdisk_id].read(partition->vdisk_id, buf, number, partition->starting_lba + lba);
}

size_t partition_write(int part, uint8_t *buf, size_t number, size_t lba)
{
    partition_t *partition = device_lists[part];
    return device_ctl[partition->vdisk_id].write(partition->vdisk_id, buf, number, partition->starting_lba + lba);
}

static int partition_sync(partition_t *partition)
{
    if (partition == NULL) return -ENODEV;
    if (partition->vdisk_id >= 256) return -ENODEV;
    device_t *base = &device_ctl[partition->vdisk_id];
    if (base->flag < 1 || base->ioctl == NULL) return -ENODEV;
    return base->ioctl(base, IOBLKSYNC, NULL);
}

void format_guid(const uint8_t guid[16], char out[37])
{
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", guid[3], guid[2], guid[1],
             guid[0],                                                   // time_low
             guid[5], guid[4],                                          // time_mid
             guid[7], guid[6],                                          // time_hi_and_version
             guid[8], guid[9],                                          // clock_seq
             guid[10], guid[11], guid[12], guid[13], guid[14], guid[15] // node
    );
}

bool is_partition_used(struct GPT_DPTE *entry)
{
    if (entry->starting_lba == 0 || entry->ending_lba == 0) return false;

    const uint8_t *partition_type_guid = entry->partition_type_guid;
    for (int i = 0; i < 16; ++i)
    {
        if (partition_type_guid[i] != 0) return true;
    }
    return false;
}

static bool partition_lba_range_is_valid(uint64_t starting_lba, uint64_t ending_lba, size_t sector_size,
                                         size_t disk_size)
{
    if (sector_size == 0 || starting_lba > ending_lba) return false;

    size_t disk_sectors = disk_size / sector_size;
    if (disk_sectors == 0 || ending_lba >= disk_sectors) return false;
    uint64_t size_max = (uint64_t)(~(size_t)0);
    if (starting_lba > size_max || ending_lba > size_max) return false;

    uint64_t sector_count = ending_lba - starting_lba + 1;
    return sector_count <= size_max / sector_size;
}

static bool parse_gpt_partitions(vfs_node_t device, struct GPT_DPT *gpt, device_t disk, size_t vdisk_id)
{
    if (memcmp(gpt->signature, "EFI PART", 8) != 0 || gpt->num_partition_entries == 0 ||
        gpt->partition_entry_lba == 0 || gpt->size_of_partition_entry < sizeof(struct GPT_DPTE) ||
        gpt->size_of_partition_entry > 512)
    {
        return false;
    }

    if (partition_num >= MAX_PARTITIONS_NUM ||
        gpt->num_partition_entries > MAX_PARTITIONS_NUM - partition_num ||
        gpt->num_partition_entries > (~(size_t)0) / gpt->size_of_partition_entry ||
        disk.sector_size == 0 || gpt->partition_entry_lba > (~(size_t)0) / disk.sector_size)
        return false;

    size_t dptes_size = (size_t)gpt->num_partition_entries * gpt->size_of_partition_entry;
    size_t entries_offset = (size_t)gpt->partition_entry_lba * disk.sector_size;
    if (entries_offset > disk.size || dptes_size > disk.size - entries_offset) return false;

    size_t disk_sectors = disk.size / disk.sector_size;
    if (gpt->first_usable_lba == 0 || gpt->first_usable_lba > gpt->last_usable_lba ||
        gpt->last_usable_lba >= disk_sectors)
        return false;

    struct GPT_DPTE *dptes      = (struct GPT_DPTE *)malloc(dptes_size);
    if (!dptes) return false;
    memset(dptes, 0, dptes_size);

    if (!partition_read_exact(device, disk, (uint8_t *)dptes, entries_offset, dptes_size,
                              "GPT entries"))
    {
        free(dptes);
        return false;
    }

    for (size_t j = 0; j < gpt->num_partition_entries; j++)
    {
        struct GPT_DPTE *entry = (struct GPT_DPTE *)((uint8_t *)dptes + j * gpt->size_of_partition_entry);
        if (is_partition_used(entry))
        {
            if (entry->starting_lba < gpt->first_usable_lba || entry->ending_lba > gpt->last_usable_lba ||
                !partition_lba_range_is_valid(entry->starting_lba, entry->ending_lba, disk.sector_size, disk.size))
            {
                pr_warn("PART: ignoring invalid GPT entry %zu on %s (lba=%llu..%llu)\n", j, disk.drive_name,
                        entry->starting_lba, entry->ending_lba);
                continue;
            }

            partition_t *partition  = &partitions[partition_num];
            partition->vdisk_id     = vdisk_id;
            partition->starting_lba = entry->starting_lba;
            partition->ending_lba   = entry->ending_lba;
            partition->type         = partition->GPT;
            partition->sector_size  = disk.sector_size;
            partition->is_used      = true;
            memcpy(partition->disk_guid, gpt->disk_guid, 16);
            memcpy(partition->partition_type_guid, entry->partition_type_guid, 16);
            memcpy(partition->unique_partition_guid, entry->unique_partition_guid, 16);
            memcpy(partition->partition_name, entry->partition_name, 36 * 2);
            partition_num++;
            char out[37];
            format_guid(entry->unique_partition_guid, out);
            pr_info("GPT Partition(%s) %zu GUID: %s\n", disk.drive_name, partition_num, out);
        }
    }

    free(dptes);
    return true;
}
// Try GPT header directly at LBA1.
static bool try_gpt_at_lba1(vfs_node_t device, device_t disk, size_t vdisk_id)
{
    if (disk.sector_size < sizeof(struct GPT_DPT) || disk.sector_size > (~(size_t)0) / 2) return false;
    struct GPT_DPT *gpt = (struct GPT_DPT *)malloc(disk.sector_size);
    if (!gpt) return false;
    memset(gpt, 0, disk.sector_size);

    if (!partition_read_exact(device, disk, (uint8_t *)gpt, disk.sector_size, disk.sector_size, "GPT header"))
    {
        free(gpt);
        return false;
    }

    bool is_valid = (memcmp(gpt->signature, "EFI PART", 8) == 0);
    if (is_valid)
    {
        pr_info("Detected GPT at LBA1 (e.g., hybrid ISO)");
        bool ok = parse_gpt_partitions(device, gpt, disk, vdisk_id);
        free(gpt);
        return ok;
    }

    free(gpt);
    return false;
}
bool parser_block_device(vfs_node_t device, device_t disk, size_t vdisk_id)
{
    if (disk.sector_size < 512 || disk.sector_size > (~(size_t)0) / 4 || disk.size < disk.sector_size * 4)
        return false;
    uint8_t *mbr = (uint8_t *)malloc(disk.sector_size * 4);
    if (mbr == NULL) return false;
    memset(mbr,0,disk.sector_size*4);
    pr_info("PART: read %s sector_size=%zu size=0x%zx\n", disk.drive_name, disk.sector_size, disk.size);
    bool read_ok = false;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        memset(mbr, 0, disk.sector_size * 4);
        if (partition_read_exact(device, disk, mbr, 0, disk.sector_size * 4, "MBR"))
        {
            read_ok = true;
            pr_info("PART: MBR read attempt=%d signature=%02x%02x type0=%02x\n", attempt + 1, mbr[0x1FE],
                             mbr[0x1FF], mbr[0x1BE + 4]);
            if (mbr[0x1FE] == 0x55 && mbr[0x1FF] == 0xAA) break;
        }
        else
        {
            pr_info("PART: retrying %s after failed read attempt=%d\n", disk.drive_name, attempt + 1);
        }
        delay_ms_hp(10);
    }
    if (!read_ok) { free(mbr); return false; }
    // for(int i=0;i<disk.sector_size*4;i++)
    // {
    //     if(i%32==0 && i!=0)
    //     {
    //         pr_info("\n");
    //         delay_ms_hp(500);
    //     }
    //     pr_info("0x%x ", mbr[i]);
    // }
    // pr_info("\n");
    pr_info("PART: MBR signature=%02x%02x type0=%02x\n", mbr[0x1FE], mbr[0x1FF], mbr[0x1BE + 4]);
    if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA) {
        partition_dump_prefix(disk.drive_name, mbr, disk.sector_size);
    }
    if (mbr[0x1FE] == 0x55 && mbr[0x1FF] == 0xAA)
    {
        uint8_t part_type = mbr[0x1BE + 4];
        if (part_type == 0xEE)
        {
            struct GPT_DPT *gpt = (struct GPT_DPT *)malloc(disk.sector_size);
            if (gpt == NULL) { free(mbr); return false; }
            memset(gpt, 0, disk.sector_size);
            if (!partition_read_exact(device, disk, gpt, 1 * disk.sector_size, disk.sector_size, "GPT header"))
            {
                free(gpt);
                free(mbr);
                return false;
            }
            pr_info("PART: GPT sig=%.8s rev=0x%x hdr=%u pe_lba=%llu entries=%u entry_size=%u\n",
                             gpt->signature, gpt->revision, gpt->header_size, gpt->partition_entry_lba,
                             gpt->num_partition_entries, gpt->size_of_partition_entry);
            bool parsed = parse_gpt_partitions(device, gpt, disk, vdisk_id);
            free(gpt);
            if (!parsed) { free(mbr); return false; }
        }
        else
        {
            struct MBR_DPT *boot_sector = (struct MBR_DPT *)mbr;
            if (boot_sector->bs_trail_sig != 0xAA55) { goto end; }
            for (int j = 0; j < MBR_MAX_PARTITION_NUM; j++)
            {
                if (boot_sector->dpte[j].start_lba == 0 || boot_sector->dpte[j].sectors_limit == 0) continue;
                if (partition_num >= MAX_PARTITIONS_NUM) break;
                uint64_t starting_lba = boot_sector->dpte[j].start_lba;
                uint64_t sector_count = boot_sector->dpte[j].sectors_limit;
                if (sector_count - 1 > __UINT64_MAX__ - starting_lba)
                {
                    pr_warn("PART: ignoring overflowing MBR entry %d on %s\n", j, disk.drive_name);
                    continue;
                }
                uint64_t ending_lba = starting_lba + sector_count - 1;
                if (!partition_lba_range_is_valid(starting_lba, ending_lba, disk.sector_size, disk.size))
                {
                    pr_warn("PART: ignoring invalid MBR entry %d on %s (lba=%llu..%llu)\n", j, disk.drive_name,
                            starting_lba, ending_lba);
                    continue;
                }
                partition_t *partition    = &partitions[partition_num];
                partition->vdisk_id       = vdisk_id;
                partition->starting_lba   = (size_t)starting_lba;
                partition->ending_lba     = (size_t)ending_lba;
                partition->type           = partition->MBR;
                partition->sector_size    = disk.sector_size;
                partition->is_used        = true;

                pr_info("MBR Partition(%s) %d lba=%llu..%llu %s\n", disk.drive_name, j, starting_lba,
                                 ending_lba, (boot_sector->dpte[j].flags & 0x80) != 0 ? "bootable" : "");
                partition_num++;
            }
        }
    }
    else
    {
        if (try_gpt_at_lba1(device, disk, vdisk_id))
        {
            free(mbr);
            return true;
        }
    }
end:
    free(mbr);
    return true;
}

int extract_partition_index(const char *name)
{
    if (name == NULL || name[0] == '\0') return -1;

    const char *prefix     = "part";
    size_t      prefix_len = strlen(prefix);
    const char *suffix     = NULL;
    if (strncmp(name, prefix, prefix_len) == 0)
    {
        suffix = name + prefix_len;
    }
    else
    {
        size_t len = strlen(name);
        size_t pos = len;
        while (pos > 0 && isdigit((unsigned char)name[pos - 1]))
        {
            pos--;
        }
        if (pos == len || pos == 0 || name[pos - 1] != 'p') return -1;
        suffix = name + pos;
    }

    if (suffix[0] == '\0') return -1;
    for (const char *p = suffix; *p; p++)
    {
        if (!isdigit((unsigned char)*p)) { return -1; }
    }
    return atoi(suffix);
}

int partition_ioctl(device_t *device, size_t req, void *arg)
{
    int device_id = extract_partition_index(device->drive_name);
    if (device_id == -1) return -ENODEV;
    partition_t partition = partitions[device_id];
    switch (req)
    {
    case IOBLKSYNC:
        return partition_sync(&partition);
    case IOGPTYPE: {
        if (arg == NULL) return -EINVAL;
        uint32_t *type = (uint32_t *)arg;
        *type          = partition.type == partition.GPT   ? PARTITION_TYPE_GPT
                         : partition.type == partition.MBR ? PARTITION_TYPE_MBR
                                                           : PARTITION_TYPE_UNKNOWN;
        break;
    }
    case IOGPINFO: {
        if (arg == NULL) return -EINVAL;
        if (partition.type == partition.GPT)
        {
            struct ioctl_gpt_partition *info = (struct ioctl_gpt_partition *)arg;
            memcpy(info->disk_guid, partition.disk_guid, 16);
            memcpy(info->partition_type_guid, partition.partition_type_guid, 16);
            memcpy(info->unique_partition_guid, partition.unique_partition_guid, 16);
            memcpy(info->name, partition.partition_name, 36);
            info->size  = partition.sector_size;
            info->start = partition.starting_lba;
            info->end   = partition.ending_lba;
        }
    }
    break;
    }
    return EOK;
}

void partition_init()
{
    for (size_t i = 0; i < 256; i++)
    {
        if (device_ctl[i].flag >= 1 && device_ctl[i].type == DEVICE_BLOCK)
        {
            partition_scan_single_device(i, false);
        }
    }

    pr_info("Loading part device...\n");
    for (size_t j = 0; j < partition_num; j++) partition_register_device_node(j);
    partition_runtime_ready = true;
}

extern "C" void partition_device_added(size_t vdisk_id)
{
    if (!partition_runtime_ready) return;
    partition_scan_single_device(vdisk_id, true);
}
EXPORT_SYMBOL(partition_device_added);

static int root_probe_ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static bool root_probe_name_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return a == b;
    while (*a != '\0' && *b != '\0')
    {
        if (root_probe_ascii_lower((unsigned char)*a) != root_probe_ascii_lower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static vfs_node_t root_probe_find_child(vfs_node_t parent, const char *name)
{
    if (parent == NULL || name == NULL) return NULL;
    vfs_update(parent);

    vfs_node_t child = NULL;
    vfs_child_lock();
    list_foreach(parent->child, item)
    {
        vfs_node_t candidate = (vfs_node_t)item->data;
        if (root_probe_name_equals(candidate->name, name))
        {
            child = candidate;
            break;
        }
    }
    vfs_child_unlock();
    return child;
}

static bool root_probe_has_file_from(vfs_node_t root, const char *path)
{
    if (root == NULL || path == NULL || path[0] == '\0') return false;

    char *copy = strdup(path);
    if (copy == NULL) return false;

    vfs_node_t current = root;
    char      *part    = copy;
    while (*part == '/') part++;

    while (*part != '\0')
    {
        char *next = part;
        while (*next != '\0' && *next != '/') next++;
        if (*next == '/')
        {
            *next = '\0';
            next++;
            while (*next == '/') next++;
        }

        current = root_probe_find_child(current, part);
        if (current == NULL)
        {
            free(copy);
            return false;
        }

        part = next;
    }

    bool found = (current->type & file_dir) == 0;
    free(copy);
    return found;
}

static bool root_probe_has_system_markers(vfs_node_t root)
{
    bool has_boot = root_probe_has_file_from(root, "EFI/BOOT/BOOTX64.EFI");
    bool has_kernel = root_probe_has_file_from(root, "system/kernel.krl");
    return has_boot && has_kernel;
}

static bool root_probe_partition(const char *dev_path)
{
    vfs_mkdir("/__root_probe");

    vfs_node_t probe = vfs_open("/__root_probe");
    if (probe == NULL) return false;

    bool found = false;
    if (vfs_mount(dev_path, probe) == VFS_STATUS_SUCCESS)
    {
        found = root_probe_has_system_markers(probe);
        vfs_unmount("/__root_probe");
    }

    vfs_delete(probe);
    return found;
}

static bool partition_name_equals_ascii(const partition_t &partition, const char *name)
{
    if (name == NULL) return false;

    size_t i = 0;
    for (; i < 36 && name[i] != '\0'; i++)
    {
        if (partition.partition_name[i] != (uint16_t)name[i]) return false;
    }
    return name[i] == '\0' && (i >= 36 || partition.partition_name[i] == 0);
}

static bool mount_partition_index(uint64_t index, const char *reason)
{
    partition_t p       = partitions[index];
    device_t   *src_dev = get_device(p.vdisk_id);
    if (src_dev == NULL) return false;

    char buf[1024] = {};
    sprintf(buf, "/dev/%sp%zu", src_dev->drive_name, index);
    write_serial_string(buf);
    write_serial_string("\n");

    if (vfs_mount((const char *)buf, rootdir) == VFS_STATUS_SUCCESS)
    {
        pr_info("Mount root OK: %s (%s)\n", buf, reason != NULL ? reason : "system markers");
        return true;
    }

    return false;
}

static bool mount_named_root_partition(const char *name, const char *reason, const char *message)
{
    for (uint64_t i = 0; i < partition_num; i++)
    {
        if (!partition_name_equals_ascii(partitions[i], name)) continue;
        if (mount_partition_index(i, reason))
        {
            write_serial_string(message);
            write_serial_string("\n");
            return true;
        }
    }

    return false;
}

static bool mount_named_root_fallback()
{
    if (mount_named_root_partition("XJ380", "XJ380 fallback", "system not found, found XJ380")) return true;
    if (mount_named_root_partition("ESP", "ESP fallback", "system not found, found ESP")) return true;
    return false;
}

void mount_root()
{
    for (uint64_t i = 0; i < partition_num; i++)
    {
        partition_t p         = partitions[i];
        device_t   *src_dev   = get_device(p.vdisk_id);
        if (src_dev == NULL) continue;

        char        buf[1024] = {};
        sprintf(buf, "/dev/%sp%zu", src_dev->drive_name, i);
        write_serial_string(buf);
        write_serial_string("\n");

        if (!root_probe_partition(buf))
        {
            continue;
        }

        if (vfs_mount((const char *)buf, rootdir) == VFS_STATUS_SUCCESS)
        {
            pr_info("Mount root OK: %s\n", buf);
            return;
        }
    }

    if (mount_named_root_fallback()) return;

    write_serial_string("system not found\n");
    while (1)
    {
        __asm__ volatile("pause\n\t");
    }
}
