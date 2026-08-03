#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <mm/uaccess.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <power.h>
#include <rtc.h>
#include <syscall/pxapi.h>
#include <syscall/xapi_user.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <syscall/syscall.h>
#include "../../build_settings.h"
#include <user/runfile.h>
#include <user/settings.h>
#include <user/user.h>
#include <errno.h>

extern UserInfo *current_user;
extern lock_queue *pcb_group_queue;
extern EFI_SYSTEM_TABLE *EFI_ST;
extern BOOT_CONFIG *EFI_BC;

uint64_t do_xapi_GetTime()
{
    tm t;
    time_read(&t);
    return mktime(&t);
}

// Helpers prefixed with p_xapi_ are private to this translation unit.
void p_xapi_output_kernel(const char *str)
{
    if (str == NULL) return;

    pcb_t front_p = get_current_task()->parent_group;
    pcb_t caller = front_p;
    while (true)
    {
        if (front_p == NULL || front_p == kernel_group)
        {
            write_serial_string(str);
            return;
        }
        if (front_p->xtttp_stc->is_shell)
        {
            // found it!
            if (str == NULL) {
                return;
            }

            char  *output    = front_p->xtttp_stc->output;
            size_t  out_cap  = sizeof(front_p->xtttp_stc->output);
            size_t  out_len  = strlen(output);
            size_t  str_len  = strlen(str);
            size_t  copy_len = str_len;

            if (out_len < out_cap - 1)
            {
                size_t avail = out_cap - out_len - 1;
                if (copy_len > avail) copy_len = avail;
                memcpy(output + out_len, str, copy_len);
                output[out_len + copy_len] = '\0';
            }

            front_p->xtttp_stc->output_lock = false;

            while (true)
            {
                if (front_p->xtttp_stc->output_lock) break;
                scheduler_yield();
            }
            memset(front_p->xtttp_stc->output, 0, sizeof(front_p->xtttp_stc->output));
            return;
        }
        front_p = front_p->parent_task;
    }
}

static void p_xapi_free_argv(char **argv)
{
    if (argv == NULL) return;
    for (size_t i = 0; argv[i] != NULL; i++)
    {
        free(argv[i]);
    }
    free(argv);
}

static int p_xapi_copy_argv_from_user(char **user_argv, char ***out)
{
    if (out == NULL) return -EINVAL;
    *out = NULL;
    if (user_argv == NULL) return 0;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL) return -EFAULT;

    char **argv = (char **)calloc(XAPI_RUN_ARG_MAX + 1, sizeof(char *));
    if (argv == NULL) return -ENOMEM;

    for (size_t i = 0; i < XAPI_RUN_ARG_MAX; i++)
    {
        char *user_arg = NULL;
        if (!copy_from_user_pagedir(pagedir, &user_arg, &user_argv[i], sizeof(user_arg)))
        {
            p_xapi_free_argv(argv);
            return -EFAULT;
        }
        if (user_arg == NULL)
        {
            *out = argv;
            return 0;
        }
        int ret = xapi_copy_string_from_user(&argv[i], user_arg, XAPI_USER_STRING_MAX);
        if (ret < 0)
        {
            p_xapi_free_argv(argv);
            return ret;
        }
    }

    p_xapi_free_argv(argv);
    return -E2BIG;
}

static size_t p_xapi_strnlen(const char *str, size_t max_len)
{
    if (str == NULL) return 0;
    size_t len = 0;
    while (len < max_len && str[len] != '\0') len++;
    return len;
}

void do_xapi_GetSystemVersion(uint64_t str)
{
    if (str == 0) return;
    copy_to_user_pagedir(xapi_current_pagedir(), (void *)str, OS_VERSION, strlen(OS_VERSION) + 1);
}

void do_xapi_GetCurrentUser(uint64_t dst)
{
    if (dst == 0 || current_user == NULL) return;
    xapi_type_UserInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name, current_user->name, sizeof(info.name) - 1);
    info.user_type = current_user->user_type;
    copy_to_user_pagedir(xapi_current_pagedir(), (void *)dst, &info, sizeof(info));
}

uint64_t do_xapi_UserOobeRequired()
{
    return user_session_needs_oobe() ? 1 : 0;
}

uint64_t do_xapi_UserList(uint64_t buffer, uint64_t max_count)
{
    if (buffer == 0) return (uint64_t)-EINVAL;
    if (max_count > 128) max_count = 128;

    UserInfo users[128];
    memset(users, 0, sizeof(users));
    int count = user_session_list(users, (int)max_count);
    if (count < 0) return (uint64_t)count;

    xapi_type_LoginUserInfo out[128];
    memset(out, 0, sizeof(out));
    for (int i = 0; i < count; i++)
    {
        strncpy(out[i].name, users[i].name, sizeof(out[i].name) - 1);
        out[i].user_type = users[i].user_type;
    }

    if (count > 0 && !copy_to_user_pagedir(xapi_current_pagedir(), (void *)buffer, out,
                                           sizeof(xapi_type_LoginUserInfo) * (size_t)count))
    {
        return (uint64_t)-EFAULT;
    }
    return (uint64_t)count;
}

uint64_t do_xapi_UserLogin(uint64_t username, uint64_t password)
{
    char *kusername = NULL;
    char *kpassword = NULL;
    int ret = xapi_copy_string_from_user(&kusername, (const char *)username, sizeof(((UserInfo *)0)->name));
    if (ret < 0) return (uint64_t)ret;
    ret = xapi_copy_string_from_user(&kpassword, (const char *)password, sizeof(((UserInfo *)0)->password));
    if (ret < 0)
    {
        free(kusername);
        return (uint64_t)ret;
    }

    ret = user_session_login(kusername, kpassword);
    free(kusername);
    free(kpassword);
    return (uint64_t)ret;
}

uint64_t do_xapi_UserCreateFirst(uint64_t username, uint64_t password)
{
    char *kusername = NULL;
    char *kpassword = NULL;
    int ret = xapi_copy_string_from_user(&kusername, (const char *)username, sizeof(((UserInfo *)0)->name));
    if (ret < 0) return (uint64_t)ret;
    ret = xapi_copy_string_from_user(&kpassword, (const char *)password, sizeof(((UserInfo *)0)->password));
    if (ret < 0)
    {
        free(kusername);
        return (uint64_t)ret;
    }

    ret = user_session_create_first(kusername, kpassword);
    free(kusername);
    free(kpassword);
    return (uint64_t)ret;
}

void do_xapi_Output(char *str)
{
    char *kstr = NULL;
    if (xapi_copy_string_from_user(&kstr, str, XAPI_USER_STRING_MAX) < 0) return;
    p_xapi_output_kernel(kstr);
    free(kstr);
}

int do_xapi_Input(char *str, size_t capacity, uint64_t flags)
{
    if (str == NULL) return -EFAULT;
    if (capacity == 0 || capacity > XAPI_USER_STRING_MAX) return -EINVAL;
    if ((flags & ~((uint64_t)XAPI_INPUT_NO_ECHO)) != 0) return -EINVAL;

    pcb_t front_p = get_current_task()->parent_group;
    while (true)
    {
        if (front_p == NULL || front_p == kernel_group)
        {
            char input[XAPI_USER_STRING_MAX];
            size_t index = 0;
            size_t discarded = 0;
            while (true)
            {
                uint8_t value = get_keyboard_input();
                if (value == 0) { scheduler_yield(); continue; }
                if (value == '\b')
                {
                    if (discarded > 0)
                    {
                        discarded--;
                    }
                    else if (index > 0)
                    {
                        index--;
                    }
                    else continue;
                    if ((flags & XAPI_INPUT_NO_ECHO) == 0) write_serial_string("\b \b");
                    continue;
                }
                if (value == '\n') { write_serial_string("\n"); break; }
                if (value >= 32 && value < 127)
                {
                    if (index < capacity - 1) input[index++] = (char)value;
                    else if (discarded != (~(size_t)0)) discarded++;
                    if ((flags & XAPI_INPUT_NO_ECHO) == 0)
                    {
                        char echo[2] = {(char)value, '\0'};
                        write_serial_string(echo);
                    }
                }
            }
            input[index] = '\0';
            bool copied = copy_to_user_pagedir(xapi_current_pagedir(), str, input, index + 1);
            if ((flags & XAPI_INPUT_NO_ECHO) != 0) memset(input, 0, sizeof(input));
            return copied ? 0 : -EFAULT;
        }
        if (front_p->xtttp_stc->is_shell)
        {
            front_p->xtttp_stc->input_flags = (uint32_t)flags;
            front_p->xtttp_stc->wait_for_input = true;
            // 等待命令行输入
            while (true)
            {
                if (front_p->xtttp_stc->input_lock) break;
                scheduler_yield();
            }
            size_t input_len = p_xapi_strnlen(front_p->xtttp_stc->input, sizeof(front_p->xtttp_stc->input) - 1);
            while (input_len > 0 && (front_p->xtttp_stc->input[input_len - 1] == '\n' ||
                                     front_p->xtttp_stc->input[input_len - 1] == '\r'))
                input_len--;
            front_p->xtttp_stc->input[input_len] = '\0';
            if (input_len >= capacity) input_len = capacity - 1;
            bool copied = copy_to_user_pagedir(xapi_current_pagedir(), str, front_p->xtttp_stc->input, input_len);
            char terminator = '\0';
            if (copied)
                copied = copy_to_user_pagedir(xapi_current_pagedir(), str + input_len, &terminator, 1);
            memset(front_p->xtttp_stc->input, 0, sizeof(front_p->xtttp_stc->input));
            front_p->xtttp_stc->input_lock = false;
            front_p->xtttp_stc->wait_for_input = false;
            front_p->xtttp_stc->input_flags = 0;
            return copied ? 0 : -EFAULT;
        }
        front_p = front_p->parent_task;
    }
}

char do_xapi_Getch()
{
    pcb_t front_p = get_current_task()->parent_group;
    while (true)
    {
        if (front_p == NULL || front_p == kernel_group)
        {
            pr_warn("A program call xapi_Getch, but cannot found parent terminal. \n");
            return 0;
        }
        if (front_p->xtttp_stc->is_shell)
        {
            // 等待命令行输入
            front_p->xtttp_stc->wait_for_getch = true;
            while (true)
            {
                if (!front_p->xtttp_stc->wait_for_getch) break;
                scheduler_yield();
            }
            // found it!
            return front_p->xtttp_stc->char_for_getch;
        }
        front_p = front_p->parent_task;
    }
}

void do_xapi_Endline()
{
    p_xapi_output_kernel("\n");
}

void do_xapi_Printline(char *str)
{
    char *kstr = NULL;
    if (xapi_copy_string_from_user(&kstr, str, XAPI_USER_STRING_MAX) < 0) return;
    size_t len = p_xapi_strnlen(kstr, XAPI_USER_STRING_MAX - 2);
    kstr[len] = '\n';
    kstr[len + 1] = '\0';
    p_xapi_output_kernel(kstr);
    free(kstr);
}

void do_xapi_OutputSerial(char *str)
{
    char *kstr = NULL;
    if (xapi_copy_string_from_user(&kstr, str, XAPI_USER_STRING_MAX) < 0) return;
    tcb_t task = get_current_task();
    write_serial_fmt("[xapi-serial task=%s ptr=0x%llx] ",
                     task != NULL ? task->name : "<none>",
                     (unsigned long long)(uint64_t)str);
    write_serial_string(kstr);
    free(kstr);
}

void do_xapi_Printf(char *str)
{
    do_xapi_Output(str);
}

void do_xapi_Sleep(uint64_t ms)
{
    uint64_t nano = ms * 1000000ULL;
    if (ms != 0 && nano / 1000000ULL != ms) nano = (uint64_t)-1;
    scheduler_sleep_ns(nano);
}

void do_xapi_GetTimeX(uint64_t tt)
{
    if (!tt) { return; }
    tm local;
    memset(&local, 0, sizeof(local));
    time_read(&local);
    copy_to_user_pagedir(get_current_task()->parent_group->pagedir, (void *)tt, &local, sizeof(local));
}

extern uint64_t memory_total_size;

uint64_t do_xapi_GetMemorySize()
{
    return memory_total_size / 1024;
}

void do_xapi_Run(char *path)
{
    page_directory_t *caller_pagedir = get_current_task()->parent_group->pagedir;
    char *upath = NULL;
    if (xapi_copy_string_from_user(&upath, path, XAPI_USER_PATH_MAX) < 0) return;
    char  *fpath = vfs_cwd_path_build(upath);
    free(upath);
    if (fpath == NULL) return;
    runfile(fpath);
    switch_page_directory(caller_pagedir);
    free(fpath);
}

uint64_t do_xapi_RunArgs(char *path, char **argv)
{
    page_directory_t *caller_pagedir = get_current_task()->parent_group->pagedir;
    char *upath = NULL;
    int ret = xapi_copy_string_from_user(&upath, path, XAPI_USER_PATH_MAX);
    if (ret < 0) return (uint64_t)ret;

    char *fpath = vfs_cwd_path_build(upath);
    free(upath);
    if (fpath == NULL) return (uint64_t)-ENOENT;

    char **kargv = NULL;
    ret = p_xapi_copy_argv_from_user(argv, &kargv);
    if (ret < 0)
    {
        free(fpath);
        return (uint64_t)ret;
    }
    write_serial_fmt("[busybox-debug] do_xapi_RunArgs fpath=%s\n", fpath);
    if (kargv != NULL)
    {
        for (int i = 0; kargv[i] != NULL; i++)
        {
            write_serial_fmt("[busybox-debug] do_xapi_RunArgs argv[%d]=%s\n", i, kargv[i]);
        }
    }

    ret = create_user_process_from_file(fpath, get_current_task()->parent_group, kargv);
    switch_page_directory(caller_pagedir);
    p_xapi_free_argv(kargv);
    free(fpath);
    return (uint64_t)ret;
}

uint64_t do_xapi_MapMemory(uint64_t ptr, uint64_t size, uint32_t flags)
{
    uint64_t ret = -1;
    if (size == 0 || check_user_overflow(ptr, size)) return ret;
    flags |= PTE_USER;
    if (ptr == NULL)
    {
        ret = page_alloc_random(get_current_task()->parent_group->pagedir, size, flags);
    }
    else
    {
        page_map_range_to_random(get_current_task()->parent_group->pagedir, ptr, size, flags);
        ret = ptr;
    }
    return ret;
}
