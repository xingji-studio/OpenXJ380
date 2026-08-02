#include "build_settings.h"
#include "build_config.h"
#include <ahci/ahci.h>
#include <cpu/regio.h>
#include <device.h>
#include <cpu/fsgsbase.h>
#include <dlinker.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <font.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fbdev.h>
#include <fs/vfs/vfs.h>
#include <global_color.h>
#include <graphics/GOP.hpp>
#include <graphics/sheet.h>
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
#include <pty.h>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <sb16.h>
#include <syscall/signal.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <ttf.h>
#include <user/runfile.h>
#include <user/info_register.h>
#include <hda/vsound.h>
#include <hda/hda.h>
#include <installer_mode.h>

void TEMP_stress_test_function()
{
	for (int i = 0; i < 2000; i++)
	{
    	create_user_process_from_file((char *)"/apps/builtin/picturer.elf", NULL, NULL);
	}
}