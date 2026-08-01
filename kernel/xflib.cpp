#include "build_settings.h"
#include <ahci/ahci.h>
#include <cpu/regio.h>
#include <device.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/sys.h>
#include <fs/vfs/vfs.h>
#include <hda/hda.h>
#include <hda/pcspk.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <nvme/nvme.h>
#include <pci/pci.h>
#include <pipe.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <sb16.h>
#include <syscall/signal.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <dlinker.h>

char getch_from_file(void *buffer)
{
    char *p = (char *)buffer;
    return *p;
}

// 从文件里读取字符直到结束符/换行符，返回偏移量
uint64_t getline_from_file(void *buffer, char *str)
{
    char *before = (char *)buffer;
    char *p = (char *)buffer;
    char *q = str;

    while (*p && *p != '\n' && *p != '\r')
    {    
        *q = *p;
        p++; q++;
    }

    *q = '\0';

    while (*p && (*p == '\n' || *p == '\r'))
        p++;

    return p - before;
}
