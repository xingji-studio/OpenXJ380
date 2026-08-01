#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/sys.h>
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

static constexpr uint32_t POWER_CONFIRM_WIDTH  = 420;
static constexpr uint32_t POWER_CONFIRM_HEIGHT = 142;
static constexpr uint32_t POWER_CONFIRM_OK_X1  = 238;
static constexpr uint32_t POWER_CONFIRM_OK_X2  = 328;
static constexpr uint32_t POWER_CONFIRM_BTN_Y1 = 100;
static constexpr uint32_t POWER_CONFIRM_BTN_Y2 = 126;


uint64_t do_xapi_GetTime()
{
    tm t;
    time_read(&t);
    return mktime(&t);
}

static uint64_t p_xapi_wait_process_exit(uint64_t pid)
{
    if ((int64_t)pid <= 0) return (uint64_t)-EINVAL;
    return sys_wait4(pid, NULL, 0, NULL, 0, 0, NULL);
}

// p 开头的代表 private
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

void do_xapi_Input(char *str)
{
    pcb_t front_p = get_current_task()->parent_group;
    while (true)
    {
        if (front_p == NULL || front_p == kernel_group)
        {
            if (str == NULL) return;
            char input[XAPI_USER_STRING_MAX];
            size_t index = 0;
            while (index < XAPI_USER_STRING_MAX - 1)
            {
                uint8_t value = get_keyboard_input();
                if (value == 0) { scheduler_yield(); continue; }
                if (value == '\b')
                {
                    if (index > 0) { index--; write_serial_string("\b \b"); }
                    continue;
                }
                if (value == '\n') { write_serial_string("\n"); break; }
                if (value >= 32 && value < 127)
                {
                    input[index++] = (char)value;
                    char echo[2] = {(char)value, '\0'};
                    write_serial_string(echo);
                }
            }
            input[index] = '\0';
            copy_to_user_pagedir(xapi_current_pagedir(), str, input, index + 1);
            return;
        }
        if (front_p->xtttp_stc->is_shell)
        {
            // 等待命令行输入
            front_p->xtttp_stc->wait_for_input = true;
            while (true)
            {
                if (front_p->xtttp_stc->input_lock) break;
                scheduler_yield();
            }
            size_t input_len = p_xapi_strnlen(front_p->xtttp_stc->input, sizeof(front_p->xtttp_stc->input) - 1);
            front_p->xtttp_stc->input[input_len] = '\0';
            copy_to_user_pagedir(xapi_current_pagedir(), str, front_p->xtttp_stc->input, input_len + 1);
            memset(front_p->xtttp_stc->input, 0, sizeof(front_p->xtttp_stc->input));
            front_p->xtttp_stc->input_lock = false;
            front_p->xtttp_stc->wait_for_input = false;
            return;
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

#if 0
uint64_t do_xapi_FileDialog(uint64_t mode, uint64_t title, uint64_t initial_path, uint64_t out_path, uint64_t out_size)
{
    if (out_path == 0 || out_size == 0) return (uint64_t)-EINVAL;

    page_directory_t *caller_pagedir = get_current_task()->parent_group->pagedir;
    char *dialog_title = NULL;
    char *dialog_path = NULL;

    if (title != 0)
    {
        int ret = xapi_copy_string_from_user(&dialog_title, (const char *)title, XAPI_USER_STRING_MAX);
        if (ret < 0) return (uint64_t)ret;
    }
    if (initial_path != 0)
    {
        int ret = xapi_copy_string_from_user(&dialog_path, (const char *)initial_path, XAPI_USER_PATH_MAX);
        if (ret < 0)
        {
            free(dialog_title);
            return (uint64_t)ret;
        }
    }

    char mode_buf[16];
    snprintf(mode_buf, sizeof(mode_buf), "%llu", (unsigned long long)mode);
    char *argv[5];
    argv[0] = (char *)"/apps/system/filedlg.elf";
    argv[1] = mode_buf;
    argv[2] = dialog_title != NULL ? dialog_title : (char *)"";
    argv[3] = dialog_path != NULL ? dialog_path : (char *)"";
    argv[4] = NULL;

    int pid = create_user_process_from_file((char *)"/apps/system/filedlg.elf", get_current_task()->parent_group, argv);
    switch_page_directory(caller_pagedir);
    free(dialog_title);
    free(dialog_path);
    if (pid < 0) return (uint64_t)pid;

    uint64_t wait_ret = p_xapi_wait_process_exit((uint64_t)pid);
    switch_page_directory(caller_pagedir);
    if ((int64_t)wait_ret < 0) return wait_ret;

    char result_path[256];
    memset(result_path, 0, sizeof(result_path));
    snprintf(result_path, sizeof(result_path), "/tmp/filedlg_%d.out", pid);

    vfs_node_t node = vfs_open(result_path);
    if (node == NULL) return (uint64_t)-ENOENT;

    size_t copy_bytes = MIN((size_t)out_size, (size_t)node->size + 1);
    char *buffer = (char *)calloc(copy_bytes, 1);
    if (buffer == NULL)
    {
        vfs_close(node);
        return (uint64_t)-ENOMEM;
    }

    if (copy_bytes > 1) vfs_read(node, buffer, 0, copy_bytes - 1);
    buffer[copy_bytes - 1] = '\0';
    vfs_close(node);

    bool ok = copy_to_user_pagedir(xapi_current_pagedir(), (void *)out_path, buffer, copy_bytes);
    free(buffer);
    vfs_node_t cleanup_node = vfs_open_no_follow(result_path);
    if (cleanup_node != NULL) vfs_delete(cleanup_node);
    if (!ok) return (uint64_t)-EFAULT;
    return 0;
}

extern int clock_hour_offset;

void do_xapi_FlushTime()
{
    if (current_user == NULL) return;

    char setfile_path[256];
    memset(setfile_path, 0, 256);
    strcat(setfile_path, "/users/");
    strcat(setfile_path, current_user->name);
    strcat(setfile_path, "/settings.dat");
    vfs_node_t v = vfs_open(setfile_path);
    if (!v) return;

    SettingsDataFileFormat sdff;
    memset(&sdff, 0, sizeof(sdff));
    size_t read_size = v->size < sizeof(sdff) ? (size_t)v->size : sizeof(sdff);
    if (read_size == 0 || vfs_read(v, &sdff, 0, read_size) == -1)
    {
        vfs_close(v);
        return;
    }

    clock_hour_offset = sdff.ClockHourOffset;

    if (clock_hour_offset < 0)
        clock_hour_offset = 0;

    vfs_close(v);
}

static uint64_t taskmgr_process_memory_bytes(pcb_t process, uint64_t thread_count)
{
    if (process == NULL) return 0;

    uint64_t bytes = process->vma_manager.vm_used;
    bytes += thread_count * (BIG_USER_STACK + KERNEL_STACK_SIZE * 2);
    return bytes;
}

static void taskmgr_copy_name(char *dst, const char *src)
{
    if (dst == NULL) return;
    memset(dst, 0, XAPI_TASK_NAME_LEN);
    if (src == NULL) return;
    strncpy(dst, src, XAPI_TASK_NAME_LEN - 1);
}

static uint64_t taskmgr_count_windows(pcb_t process, uint64_t *thread_count)
{
    if (thread_count != NULL) *thread_count = 0;
    if (process == NULL || process->thread_queue == NULL) return 0;

    uint64_t windows = 0;
    spin_lock(&process->thread_queue->lock);
    queue_foreach(process->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread == NULL) continue;
        windows += thread->window_count;
        if (thread_count != NULL) (*thread_count)++;
    }
    spin_unlock(&process->thread_queue->lock);
    return windows;
}

static void taskmgr_fill_info(XapiTaskInfo *info, pcb_t process, tcb_t thread, uint64_t thread_count,
                              uint64_t window_count, uint64_t memory_bytes)
{
    memset(info, 0, sizeof(XapiTaskInfo));
    info->pid            = process->pid;
    info->ppid           = process->ppid;
    info->tid            = thread != NULL ? thread->tid : 0;
    info->cpu_id         = thread != NULL ? thread->cpu_id : 0;
    info->task_level     = thread != NULL ? thread->task_level : process->task_level;
    info->thread_count   = thread_count;
    info->window_count   = window_count;
    info->memory_bytes   = memory_bytes;
    info->process_status = process->status;
    info->thread_status  = thread != NULL ? thread->status : process->status;
    taskmgr_copy_name(info->process_name, process->name);
    taskmgr_copy_name(info->thread_name, thread != NULL ? thread->name : process->name);
}

uint64_t do_xapi_GetTaskList(uint64_t buffer, uint64_t max_count)
{
    if (buffer == 0 || max_count == 0 || pcb_group_queue == NULL) return 0;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL) return 0;

    XapiTaskInfo *out     = (XapiTaskInfo *)buffer;
    uint64_t      written = 0;
    bool          copy_fault = false;

    spin_lock(&pcb_group_queue->lock);
    queue_foreach(pcb_group_queue, pnode)
    {
        pcb_t process = (pcb_t)pnode->data;
        if (process == NULL) continue;

        uint64_t thread_count = 0;
        uint64_t window_count = taskmgr_count_windows(process, &thread_count);
        uint64_t memory_bytes = taskmgr_process_memory_bytes(process, thread_count);

        if (process->thread_queue == NULL || thread_count == 0)
        {
            if (written >= max_count) break;
            XapiTaskInfo info;
            taskmgr_fill_info(&info, process, NULL, thread_count, window_count, memory_bytes);
            if (!copy_to_user_pagedir(pagedir, &out[written], &info, sizeof(info)))
            {
                copy_fault = true;
                break;
            }
            written++;
            continue;
        }
        if (copy_fault) break;

        spin_lock(&process->thread_queue->lock);
        queue_foreach(process->thread_queue, tnode)
        {
            if (written >= max_count) break;
            tcb_t thread = (tcb_t)tnode->data;
            if (thread == NULL) continue;
            XapiTaskInfo info;
            taskmgr_fill_info(&info, process, thread, thread_count, window_count, memory_bytes);
            if (!copy_to_user_pagedir(pagedir, &out[written], &info, sizeof(info)))
            {
                copy_fault = true;
                break;
            }
            written++;
        }
        spin_unlock(&process->thread_queue->lock);

        if (written >= max_count || copy_fault) break;
    }
    spin_unlock(&pcb_group_queue->lock);

    return written;
}

uint64_t do_xapi_KillProcess(uint64_t pid)
{
    if (kernel_group == NULL) return (uint64_t)-ESRCH;

    pcb_t current = get_current_task()->parent_group;
    pcb_t target  = found_pcb((int)pid);
    if (target == NULL || target->status == DEATH || target->status == OUT) return (uint64_t)-ESRCH;
    if (target == kernel_group || target->task_level == TASK_KERNEL_LEVEL) return (uint64_t)-EPERM;
    if (target == current) return (uint64_t)-EPERM;

    return kill_proc_deferred(target, -1) ? 0 : (uint64_t)-ENOMEM;
}

static void xapi_draw_power_confirm(SHEET *sheet, uint64_t action)
{
    if (sheet == NULL) return;

    const char *title = action == XPOWER_REBOOT ? "确认重启" : "确认关机";
    const char *body  = action == XPOWER_REBOOT ? "系统将立即重启。未保存的数据可能丢失。"
                                                : "系统将立即关闭电源。未保存的数据可能丢失。";

    draw_rect(sht_img, sheet, 0, 0, POWER_CONFIRM_WIDTH - 1, POWER_CONFIRM_HEIGHT - 1, {0xff, 0xff, 0xff, 0xff});
    draw_rect(sht_img, sheet, 0, 0, POWER_CONFIRM_WIDTH - 1, 0, {0x28, 0x78, 0xf0, 0xff});
    draw_rect(sht_img, sheet, 0, POWER_CONFIRM_HEIGHT - 1, POWER_CONFIRM_WIDTH - 1, POWER_CONFIRM_HEIGHT - 1,
              {0xd8, 0xe0, 0xeb, 0xff});
    draw_rect(sht_img, sheet, 0, 0, 0, POWER_CONFIRM_HEIGHT - 1, {0xd8, 0xe0, 0xeb, 0xff});
    draw_rect(sht_img, sheet, POWER_CONFIRM_WIDTH - 1, 0, POWER_CONFIRM_WIDTH - 1, POWER_CONFIRM_HEIGHT - 1,
              {0xd8, 0xe0, 0xeb, 0xff});

    print_box_ttf(sht_img, sheet, (char *)title, {0x15, 0x22, 0x35, 0xff}, 24, 20, 16);
    print_box_ttf(sht_img, sheet, (char *)body, {0x60, 0x70, 0x85, 0xff}, 24, 54, 11);
    print_box_ttf(sht_img, sheet, (char *)"按 Enter 确认，Esc 取消", {0x60, 0x70, 0x85, 0xff}, 24, 80, 10);

    draw_rect(sht_img, sheet, POWER_CONFIRM_OK_X1, POWER_CONFIRM_BTN_Y1, POWER_CONFIRM_OK_X2, POWER_CONFIRM_BTN_Y2,
              {0x28, 0x78, 0xf0, 0xff});
    print_box_ttf(sht_img, sheet, (char *)"确认", {0xff, 0xff, 0xff, 0xff}, POWER_CONFIRM_OK_X1 + 28,
                  POWER_CONFIRM_BTN_Y1 + 6, 10);

    draw_rect(sht_img, sheet, POWER_CONFIRM_OK_X2 + 12, POWER_CONFIRM_BTN_Y1, POWER_CONFIRM_OK_X2 + 102,
              POWER_CONFIRM_BTN_Y2, {0xee, 0xf2, 0xf7, 0xff});
    print_box_ttf(sht_img, sheet, (char *)"取消", {0x15, 0x22, 0x35, 0xff}, POWER_CONFIRM_OK_X2 + 40,
                  POWER_CONFIRM_BTN_Y1 + 6, 10);
}

static bool xapi_confirm_power_action(uint64_t action)
{
    if (sht_img == NULL) return false;

    uint32_t x = 0;
    uint32_t y = 0;
    if (sht_img->scrx > POWER_CONFIRM_WIDTH) x = (sht_img->scrx - POWER_CONFIRM_WIDTH) / 2;
    if (sht_img->scry > POWER_CONFIRM_HEIGHT) y = (sht_img->scry - POWER_CONFIRM_HEIGHT) / 2;

    SHEET *confirm_sheet = NULL;
    if (!create_sheet(sht_img, x, y, POWER_CONFIRM_WIDTH, POWER_CONFIRM_HEIGHT, TopWindowSheetType, 32765,
                      &confirm_sheet))
    {
        return false;
    }

    SHEET *previous_focus = ms_dec.sht_now;
    ms_dec.sht_now       = confirm_sheet;

    xapi_draw_power_confirm(confirm_sheet, action);
    refresh_part_sheet(sht_img, x, y, x + POWER_CONFIRM_WIDTH, y + POWER_CONFIRM_HEIGHT);

    bool confirmed = false;
    open_interrupt;
    while (true)
    {
        uint8_t key = get_keyboard_input();
        if (key == SCANCODE_ENTER || key == CHARACTER_ENTER || key == KEY_ENTER)
        {
            confirmed = true;
            break;
        }
        if (key == 1 || key == KEY_ESC)
        {
            break;
        }
        scheduler_yield();
    }
    close_interrupt;

    if (ms_dec.sht_now == confirm_sheet) ms_dec.sht_now = previous_focus;
    delete_sheet(sht_img, confirm_sheet);
    refresh_part_sheet(sht_img, x, y, x + POWER_CONFIRM_WIDTH, y + POWER_CONFIRM_HEIGHT);
    return confirmed;
}

uint64_t do_xapi_PowerAction(uint64_t action)
{
    if (EFI_ST == NULL || EFI_BC == NULL) return (uint64_t)-ENODEV;

    if (action == XPOWER_REBOOT)
    {
        if (!xapi_confirm_power_action(action)) return (uint64_t)-EINTR;
        power_reboot(EFI_ST, EFI_BC);
    }
    if (action == XPOWER_SHUTDOWN)
    {
        if (!xapi_confirm_power_action(action)) return (uint64_t)-EINTR;
        power_shutdown(EFI_ST, EFI_BC);
    }

    return (uint64_t)-EINVAL;
}
#endif
