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
#include <mm/uaccess.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
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
    pcb->xtttp_stc->input_flags = 0;
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
    if (current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL ||
        !current->parent_group->xtttp_stc->wait_for_input)
        return 0;

    int status = XTTTP_INPUT_STATUS_WAITING;
    if ((current->parent_group->xtttp_stc->input_flags & XAPI_INPUT_NO_ECHO) != 0)
        status |= XTTTP_INPUT_STATUS_NO_ECHO;
    return status;
}

int read_terminal_app_output_buffer(char *str)
{
    tcb_t current = get_current_task();
    if (str == NULL || current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL) {
        return -EFAULT;
    }

    char output[1024];
    memset(output, 0, sizeof(output));
    if (current->parent_group->xtttp_stc->output_lock)
    {
        return copy_to_user_pagedir(current->parent_group->pagedir, str, output, 1) ? 0 : -EFAULT;
    }

    strncpy(output, current->parent_group->xtttp_stc->output, sizeof(output) - 1);
    return copy_to_user_pagedir(current->parent_group->pagedir, str, output, sizeof(output)) ? 0 : -EFAULT;
}

int write_terminal_app_output_buffer(const char *str)
{
    tcb_t current = get_current_task();
    if (str == NULL || current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL)
        return -EFAULT;

    xtttp_dtt *xtttp = current->parent_group->xtttp_stc;
    size_t cur = strnlen(xtttp->input, sizeof(xtttp->input));
    if (cur >= sizeof(xtttp->input)) cur = 0;
    size_t space = sizeof(xtttp->input) - cur - 1;
    size_t len = 0;
    while (len < space)
    {
        char ch;
        if (!copy_from_user_pagedir(current->parent_group->pagedir, &ch, str + len, 1)) return -EFAULT;
        if (ch == '\0') break;
        xtttp->input[cur + len++] = ch;
    }
    xtttp->input[cur + len] = '\0';
    current->parent_group->xtttp_stc->input_lock = true;
    return 0;
}

void terminal_finish_app_output()
{
    tcb_t current = get_current_task();
    current->parent_group->xtttp_stc->output_lock = true;
}
