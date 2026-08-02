#include "../build_settings.h"
#include <ahci/ahci.h>
#include <cpu/regio.h>
#include <device.h>
#include <cpu/fsgsbase.h>
#include <dlinker.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <hda/hda.h>
#include <hda/pcspk.h>
#include <krlibc.h>
#include <mm/alloc/alloc.h>
#include <mm/frame.h>
#include <nvme/nvme.h>
#include <pci/pci.h>
#include <pipe.h>
#include <power.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <sb16.h>
#include <syscall/signal.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/runfile.h>
#include <hda/vsound.h>
#include <hda/hda.h>

extern uint64_t memory_total_size;

// XJ380 使用 XTB ( XJ380 块状文本编辑语言 ) 记录硬件信息。

uint64_t    hwr_file_offset = 0;
vfs_node_t  hwr_file_v = nullptr;

void hwr_write_char(char ch)
{
    if (hwr_file_v == nullptr) return;

    char *p = &ch;
    vfs_write(hwr_file_v, p, hwr_file_offset, sizeof(char));
    hwr_file_offset += sizeof(char);
}

void hwr_write_string(char *str)
{
    if (str == nullptr) return;

    char *p = str;
    while (*p) 
    {
        hwr_write_char(*p);
        p++;
    }
}

void hwr_create_textblock_head(char *block_name)
{
    hwr_write_char('\"');
    hwr_write_string(block_name);
    hwr_write_string("\" \n{ \n");
}

void hwr_create_textblock_item(char *item_name, char *item_value)
{
    hwr_write_char('\"');
    hwr_write_string(item_name);
    hwr_write_string("\": \"");
    hwr_write_string(item_value);
    hwr_write_string("\" \n");
}

void hwr_create_textblock_foot()
{
    hwr_write_string("} \n");
}

void register_hardware_info()
{
    write_serial_fmt("Reading Hardware Information...\n");

    if (vfs_mkfile("/system/config/hardware.xtb"))
    {
        write_serial_fmt("Reading Hardware Information Failed. (Cannot create file)\n");
        return;
    }

    hwr_file_v = vfs_open("/system/config/hardware.xtb");
    hwr_file_offset = 0;

    hwr_create_textblock_head("OS Version");
    hwr_create_textblock_item("version", OS_VERSION);
    hwr_create_textblock_foot();

    hwr_create_textblock_head("Kernel Version");
    hwr_create_textblock_item("version", KN_VERSION);
    hwr_create_textblock_foot();

    char resolution[64];
    sprintf(resolution, "%dx%d", fbc_addr->horizontal_resolution, fbc_addr->vertical_resolution);

    hwr_create_textblock_head("Display");
    hwr_create_textblock_item("resolution", resolution);
    hwr_create_textblock_foot();

    char cpu_model[128];
    get_cpu_name(cpu_model);
    hwr_create_textblock_head("CPU");
    hwr_create_textblock_item("name", cpu_model);
    hwr_create_textblock_foot();

    int memory_size_mb = memory_total_size / (1024 * 1024);
    char memory_size_str[64];
    sprintf(memory_size_str, "%d MB", memory_size_mb);
    hwr_create_textblock_head("Memory");
    hwr_create_textblock_item("size", memory_size_str);
    hwr_create_textblock_foot();

    vfs_close(hwr_file_v);

    write_serial_fmt("Reading Hardware Information Success.\n");
}
