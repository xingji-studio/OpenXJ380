#include <cpu/msr.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <mm/alloc/alloc.h>
#include <pctable/gdt.h>
#include <pctable/idt.h>
#include <proto.hpp>
#include <stdint.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <mm/frame.h>
#include <pipe.h>

static bool message_pipe_write_handle_valid(tcb_t task, fd_file_handle **out_handle)
{
    if (out_handle != NULL) *out_handle = NULL;
    if (task == NULL || task->parent_group == NULL || task->message_pipe_write_fd < 0) return false;

    fd_file_handle *handle =
        (fd_file_handle *)queue_get(task->parent_group->file_open, (size_t)task->message_pipe_write_fd);
    if (handle == NULL || handle->node == NULL || !(handle->node->type & file_pipe) || handle->node->handle == NULL)
    {
        return false;
    }

    pipe_specific_t *spec = (pipe_specific_t *)handle->node->handle;
    if (spec == NULL || !spec->write || spec->info == NULL)
    {
        return false;
    }

    if (out_handle != NULL) *out_handle = handle;
    return true;
}

static bool notify_pipe_write_handle_valid(pcb_t process, fd_file_handle **out_handle)
{
    if (out_handle != NULL) *out_handle = NULL;
    if (process == NULL || process->file_open == NULL || process->notify_pcor_pipe_write_fd < 0) return false;

    fd_file_handle *handle =
        (fd_file_handle *)queue_get(process->file_open, (size_t)process->notify_pcor_pipe_write_fd);
    if (handle == NULL || handle->node == NULL || !(handle->node->type & file_pipe) || handle->node->handle == NULL)
    {
        return false;
    }

    pipe_specific_t *spec = (pipe_specific_t *)handle->node->handle;
    if (spec == NULL || !spec->write || spec->info == NULL)
    {
        return false;
    }

    if (out_handle != NULL) *out_handle = handle;
    return true;
}

bool init_notify_message(pcb_t process, uint64_t func)
{
    if (process == NULL || func == 0) return false;

    process->notify_pcor_func       = func;
    process->notify_pcor_registered = true;

    fd_file_handle *existing_handle = NULL;
    if (process->notify_pcor_pipe_read_fd >= 0 && process->notify_pcor_pipe_write_fd >= 0 &&
        notify_pipe_write_handle_valid(process, &existing_handle))
    {
        return true;
    }

    process->notify_pcor_pipe_read_fd  = -1;
    process->notify_pcor_pipe_write_fd = -1;
    process->notify_pcor_tid           = 0;

    int pipefd[2] = {-1, -1};
    if ((int64_t)sys_pipe2(pipefd, 0, 0, 0, 0, 0, NULL) < 0)
    {
        process->notify_pcor_registered = false;
        write_serial_string("notify: pipe create failed\n");
        return false;
    }

    tcb_t current = get_current_task();
    char *cwd = current != NULL && current->str_cwd != NULL ? strdup(current->str_cwd) : strdup("/");
    if (cwd == NULL)
    {
        sys_close(pipefd[0], 0, 0, 0, 0, 0, NULL);
        sys_close(pipefd[1], 0, 0, 0, 0, 0, NULL);
        process->notify_pcor_registered = false;
        return false;
    }

    size_t tid = create_message_thread((void *)(XJ380_PRIVATE_MESSAGE_REVERT_ADDRESS + XPSR_OFFEST),
                                       "Notify Message Thread", process, cwd, (uint64_t)pipefd[0]);
    if ((int64_t)tid < 0)
    {
        sys_close(pipefd[0], 0, 0, 0, 0, 0, NULL);
        sys_close(pipefd[1], 0, 0, 0, 0, 0, NULL);
        process->notify_pcor_registered = false;
        write_serial_string("notify: message thread create failed\n");
        return false;
    }

    process->notify_pcor_pipe_read_fd  = pipefd[0];
    process->notify_pcor_pipe_write_fd = pipefd[1];
    process->notify_pcor_tid           = tid;
    return true;
}

// Message 线程
void message_thread(uint64_t read_fd)
{
    while (true)
    {
        MessageInfoFormat mif;
        uint64_t          done = 0;

        while (done < sizeof(mif))
        {
            uint64_t ret = 0;
            __asm__ __volatile__("syscall"
                                 : "=a"(ret)
                                 : "a"(SYS_READ), "D"(read_fd), "S"((uint64_t)((uint8_t *)&mif + done)),
                                   "d"(sizeof(mif) - done)
                                 : "rcx", "r11", "memory");
            if ((int64_t)ret <= 0)
            {
                done = 0;
                break;
            }
            done += ret;
        }

        if (done == sizeof(mif) && mif.WinMpf != NULL)
        {
            mif.WinMpf(mif.msg_type, mif.hData, mif.lData);
            continue;
        }

        __asm__ __volatile__("syscall" : : "a"(XAPI_SLEEP), "D"(1UL) : "rcx", "r11", "memory");
    }
}

uint64_t message_ask(uint64_t msg_type_p, uint64_t hdatap, uint64_t ldatap, uint64_t funcp, uint64_t taskp)
{
    (void)msg_type_p;
    (void)hdatap;
    (void)ldatap;
    (void)funcp;
    (void)taskp;
    return 0;
}

void do_message(uint64_t msg_type, uint64_t hData, uint64_t lData, MsgPrcor WinMpf, tcb_t ftask)
{
    if (ftask == NULL || ftask->parent_group == NULL || ftask->parent_group->xtttp_stc == NULL) return;

    tcb_t current = ftask;

    // 看看需不需要getch?
    if (current->parent_group->xtttp_stc->wait_for_getch && msg_type == MSG_CHAR)
    {
        current->parent_group->xtttp_stc->char_for_getch = lData;
        current->parent_group->xtttp_stc->wait_for_getch = false;
    }

    // 检查是否有注册的信号处理函数
    if (WinMpf != NULL && ftask->message_pipe_write_fd >= 0)
    {
        fd_file_handle *handle = NULL;
        if (!message_pipe_write_handle_valid(ftask, &handle)) return;

        MessageInfoFormat mif;
        mif.WinMpf   = WinMpf;
        mif.msg_type = msg_type;
        mif.hData    = hData;
        mif.lData    = lData;

        vfs_write(handle->node, &mif, 0, sizeof(mif));
    }
    // 空的就不用管了
}

void init_message()
{
    size_t message_thread_size = (size_t)((uintptr_t)message_ask - (uintptr_t)message_thread);
    if (message_thread_size == 0 || message_thread_size > (PAGE_SIZE - XPSR_OFFEST))
    {
        write_serial_fmt("XJ380 Message Initialize Failed. Invalid Stub Size: %d\n", message_thread_size);
        return;
    }

    page_map_range_to_random(get_current_directory(), 
                             XJ380_PRIVATE_MESSAGE_REVERT_ADDRESS, 1, 
                             PTE_PRESENT | PTE_WRITEABLE | PTE_USER);
                             
    memcpy((void *)(XJ380_PRIVATE_MESSAGE_REVERT_ADDRESS + XPSR_OFFEST), 
           (void *)message_thread, message_thread_size);

    write_serial_fmt("XJ380 Message Initialize Success.\n");
}
