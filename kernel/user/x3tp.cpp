#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <elf.h>
#include <errno.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/sys.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <rtc.h>
#include <stdint.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/user.h>
#include "../build_settings.h"

void init_process_xtttp(pcb_t pcb)
{
    // pcb->xtttp_stc->input    = (char *)malloc(1024 * sizeof(char));
    memset(pcb->xtttp_stc->output, 0, sizeof(char) * 1024);
    // pcb->xtttp_stc->output   = (char *)malloc(1024 * sizeof(char));
    memset(pcb->xtttp_stc->input, 0, sizeof(char) * 1024);
}

int check_terminal_init_status()
{
    tcb_t current = get_current_task();
    return current->parent_group->parent_task->xtttp_stc->is_shell ? 1 : 0;
}

void mark_process_is_terminal()
{
    write_serial_fmt("Marking Terminal...\n");
    tcb_t current = get_current_task();
    init_process_xtttp(current->parent_group);
    current->parent_group->xtttp_stc->is_shell = true;
}

int check_input_waiting_status()
{
    tcb_t current = get_current_task();
    return current->parent_group->xtttp_stc->wait_for_input ? 1 : 0;
}

void read_terminal_app_output_buffer(char *str)
{
    tcb_t current = get_current_task();
    if (str == NULL || current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL) {
        return;
    }

    if (current->parent_group->xtttp_stc->output_lock)
    {
        str[0] = '\0';
        return;
    }

    strncpy(str, current->parent_group->xtttp_stc->output, 1023);
    str[1023] = '\0';
}

void write_terminal_app_output_buffer(char *str)
{
    tcb_t current = get_current_task();
    xtttp_dtt *xtttp = current->parent_group->xtttp_stc;
    size_t cur = strlen(xtttp->input);
    size_t len = strlen(str);
    if (cur >= sizeof(xtttp->input)) cur = 0;
    size_t space = sizeof(xtttp->input) - cur - 1;
    if (len > space) len = space;
    if (len > 0) memcpy(xtttp->input + cur, str, len);
    xtttp->input[cur + len] = '\0';
    current->parent_group->xtttp_stc->input_lock = true;
}

void terminal_finish_app_output()
{
    tcb_t current = get_current_task();
    current->parent_group->xtttp_stc->output_lock = true;
}
