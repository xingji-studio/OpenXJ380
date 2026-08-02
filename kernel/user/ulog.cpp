#include "../build_settings.h"
#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <elf.h>
#include <errno.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <stdint.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/user.h>

void write_ulog(char *str)
{
    vfs_node_t v = vfs_open("/system/oslog.log");
    vfs_write(v, str, v->size, strlen(str) * sizeof(char));
}

void ulog_err(char *err_type)
{
    write_ulog("[XULS][Error] Task \"");
    write_ulog(get_current_task()->name);
    write_ulog("\" Was Broken. Error Type: ");
    write_ulog(err_type);
    write_ulog("\n");

}

// 启动日志系统
void init_xuls()
{
    vfs_mkfile("/system/oslog.log");
    write_ulog("[XULS][Notice] XJ380 User Log System Initialize Success.\n");
}
