#include <input_method.h>
#include <ime_utf8.h>
static ImeTextSink ime_text_sink;
void ime_set_text_sink(ImeTextSink sink) { ime_text_sink = sink; }
static spin_t getch_lock = SPIN_INIT;
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
#include <syscall/fd_lifetime.h>
#include <syscall/message_queue.h>
#include <task/kernel_worker.h>

namespace {
constexpr size_t kMessageLimitPerPipe = 4096;
constexpr size_t kMessageLimitGlobal = 32768;
constexpr unsigned kMessageBuckets = 256;
struct PendingMessage
{
    MessageInfoFormat value;
    FdOperationRecord *operation;
    PendingMessage *next;
};
struct MessageChannel
{
    pipe_info_t *identity;
    PendingMessage *head, *tail;
    MessageChannel *bucket_next, *ready_next, *ready_prev;
    size_t count;
    bool overflow_reported;
};
spin_t message_queue_lock = SPIN_INIT;
MessageChannel *message_channels[kMessageBuckets];
MessageChannel *message_ready;
size_t message_pending_count;
bool message_worker_ready;
bool message_allocation_reported;
bool message_capacity_reported;

unsigned message_bucket(pipe_info_t *identity)
{
    return unsigned((uintptr_t(identity) >> 4) ^ (uintptr_t(identity) >> 16)) & (kMessageBuckets - 1);
}

pipe_info_t *message_pipe(FdOperationRecord *operation)
{
    fd_file_handle *handle = fd_operation_handle(operation);
    if (!handle || !handle->node || !(handle->node->type & file_pipe)) return NULL;
    auto *spec = static_cast<pipe_specific_t *>(handle->node->handle);
    return spec && spec->write ? spec->info : NULL;
}

FdOperationRecord *message_acquire_channel(tcb_t task, uint64_t &inode, int &error)
{
    error = -EBADF;
    if (!task || !task->parent_group || task->status == DEATH || task->status == OUT ||
        task->message_pipe_read_fd < 0 || task->message_pipe_write_fd < 0 ||
        task->message_pipe_read_fd == task->message_pipe_write_fd) return NULL;
    FdOperationRecord *writer = fd_operation_acquire_process(task->parent_group, task->message_pipe_write_fd, error);
    if (!writer) return NULL;
    FdOperationRecord *reader = fd_operation_acquire_process(task->parent_group, task->message_pipe_read_fd, error);
    bool valid = false;
    if (reader)
    {
        fd_file_handle *read_handle = fd_operation_handle(reader);
        fd_file_handle *write_handle = fd_operation_handle(writer);
        auto *read_spec = static_cast<pipe_specific_t *>(read_handle->node->handle);
        pipe_info_t *pipe = message_pipe(writer);
        valid = (read_handle->node->type & file_pipe) && read_spec && !read_spec->write && pipe &&
                read_spec->info == pipe && read_handle->node->inode == write_handle->node->inode &&
                read_handle->node->dev == write_handle->node->dev;
        if (valid) inode = write_handle->node->inode;
    }
    fd_operation_release(reader);
    if (valid) return writer;
    fd_operation_release(writer);
    if (!error) error = -EBADF;
    return NULL;
}

// No producer writes directly to a message pipe. This common publication point
// orders input, animation and notification records for each original endpoint.
int message_enqueue(FdOperationRecord *operation, const MessageInfoFormat &value)
{
    pipe_info_t *identity = message_pipe(operation);
    if (!identity) { fd_operation_release(operation); return -EBADF; }
    auto *packet = static_cast<PendingMessage *>(malloc(sizeof(PendingMessage)));
    auto *candidate = static_cast<MessageChannel *>(calloc(1, sizeof(MessageChannel)));
    if (!packet || !candidate)
    {
        free(packet);
        free(candidate);
        fd_operation_release(operation);
        return -ENOMEM;
    }
    *packet = {value, operation, NULL};
    int error = 0;
    bool report = false;
    spin_lock(&message_queue_lock);
    unsigned index = message_bucket(identity);
    MessageChannel *channel = message_channels[index];
    while (channel && channel->identity != identity) channel = channel->bucket_next;
    if (!message_worker_ready) error = -ENOSYS;
    else if (message_pending_count >= kMessageLimitGlobal) error = -ENOBUFS;
    else if (channel && channel->count >= kMessageLimitPerPipe) error = -EAGAIN;
    else if (!fd_operation_transfer_to_kernel(operation)) error = -EBADF;
    if (!error)
    {
        if (!channel)
        {
            channel = candidate;
            candidate = NULL;
            channel->identity = identity;
            channel->bucket_next = message_channels[index];
            message_channels[index] = channel;
            if (!message_ready)
            {
                channel->ready_next = channel->ready_prev = channel;
                message_ready = channel;
            }
            else
            {
                channel->ready_next = message_ready;
                channel->ready_prev = message_ready->ready_prev;
                channel->ready_prev->ready_next = channel;
                message_ready->ready_prev = channel;
            }
        }
        if (channel->tail) channel->tail->next = packet;
        else channel->head = packet;
        channel->tail = packet;
        ++channel->count;
        ++message_pending_count;
    }
    else if ((error == -ENOBUFS || error == -EAGAIN) && channel && !channel->overflow_reported)
    {
        channel->overflow_reported = true;
        report = true;
    }
    if ((error == -ENOBUFS || error == -EAGAIN) && !channel && !message_capacity_reported)
    {
        message_capacity_reported = true;
        report = true;
    }
    spin_unlock(&message_queue_lock);
    free(candidate);
    if (error)
    {
        free(packet);
        fd_operation_release(operation);
    }
    if (report) write_serial_string("message: pending queue full; new delivery rejected, existing FIFO retained\n");
    return error;
}

// Exactly one permanent kernel worker consumes channels. A full destination
// keeps its FIFO head and rotates; it cannot hold up unrelated applications.
bool message_delivery_pump(size_t budget)
{
    bool progress = false;
    while (budget--)
    {
        spin_lock(&message_queue_lock);
        MessageChannel *channel = message_ready;
        if (!channel) { spin_unlock(&message_queue_lock); break; }
        message_ready = channel->ready_next;
        PendingMessage *packet = channel->head;
        spin_unlock(&message_queue_lock);
        int result = fd_operation_is_retired(packet->operation) ? -EPIPE :
                     pipefs_try_write_packet(fd_operation_handle(packet->operation)->node,
                                             &packet->value, sizeof(packet->value));
        if (result == -EAGAIN) continue;
        MessageChannel *empty = NULL;
        spin_lock(&message_queue_lock);
        channel->head = packet->next;
        --channel->count;
        --message_pending_count;
        if (!channel->head)
        {
            unsigned index = message_bucket(channel->identity);
            MessageChannel **link = &message_channels[index];
            while (*link != channel) link = &(*link)->bucket_next;
            *link = channel->bucket_next;
            if (channel->ready_next == channel) message_ready = NULL;
            else
            {
                channel->ready_prev->ready_next = channel->ready_next;
                channel->ready_next->ready_prev = channel->ready_prev;
                if (message_ready == channel) message_ready = channel->ready_next;
            }
            empty = channel;
        }
        spin_unlock(&message_queue_lock);
        fd_operation_release(packet->operation);
        free(packet);
        free(empty);
        progress = true;
    }
    return progress;
}

void message_delivery_worker(void *)
{
    for (;;)
    {
        message_delivery_pump(256);
        scheduler_sleep_ns(1000000ULL);
    }
}

void message_report_failure_once(int error)
{
    if (!error || error == -EBADF || error == -ESRCH || error == -EPIPE ||
        error == -EAGAIN || error == -ENOBUFS) return;
    spin_lock(&message_queue_lock);
    bool report = !message_allocation_reported;
    message_allocation_reported = true;
    spin_unlock(&message_queue_lock);
    if (report) write_serial_fmt("message: asynchronous delivery failed: %d\n", error);
}
}

bool message_window_channel_valid(tcb_t task, uint64_t *inode_out)
{
    uint64_t inode = 0;
    int error;
    FdOperationRecord *operation = message_acquire_channel(task, inode, error);
    if (!operation) return false;
    bool valid = !fd_operation_is_retired(operation);
    fd_operation_release(operation);
    if (valid && inode_out) *inode_out = inode;
    return valid;
}

int message_post_window(tcb_t task, const MessageInfoFormat &value, uint64_t expected_inode)
{
    uint64_t inode = 0;
    int error;
    FdOperationRecord *operation = message_acquire_channel(task, inode, error);
    if (!operation) return error;
    if (expected_inode && expected_inode != inode) { fd_operation_release(operation); return -EBADF; }
    return message_enqueue(operation, value);
}

int message_post_notification(uint64_t pid, uint64_t notification_id, uint64_t action_id)
{
    uint64_t callback;
    int error;
    FdOperationRecord *operation = fd_operation_acquire_notification(pid, callback, error);
    if (!operation) return error;
    MessageInfoFormat value = {};
    value.WinMpf = reinterpret_cast<MsgPrcor>(callback);
    value.msg_type = notification_id;
    value.hData = action_id;
    return message_enqueue(operation, value);
}

static bool notify_pipe_write_handle_valid(pcb_t process)
{
    int error;
    FdOperationRecord *operation = fd_operation_acquire_process(process, process->notify_pcor_pipe_write_fd, error);
    bool valid = message_pipe(operation) != NULL;
    fd_operation_release(operation);
    return valid;
}

bool init_notify_message(pcb_t process, uint64_t func)
{
    if (process == NULL || func == 0) return false;

    process->notify_pcor_func       = func;
    process->notify_pcor_registered = true;

    if (process->notify_pcor_pipe_read_fd >= 0 && process->notify_pcor_pipe_write_fd >= 0 &&
        notify_pipe_write_handle_valid(process))
    {
        return true;
    }

    process->notify_pcor_pipe_read_fd  = -1;
    process->notify_pcor_pipe_write_fd = -1;
    process->notify_pcor_tid           = 0;

    int pipefd[2] = {-1, -1};
    if ((int64_t)sys_pipe2(pipefd, O_CLOEXEC, 0, 0, 0, 0, NULL) < 0)
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

void do_message_unfiltered(uint64_t msg_type, uint64_t hData, uint64_t lData, MsgPrcor WinMpf, tcb_t ftask)
{
    if (ftask == NULL || ftask->parent_group == NULL || ftask->parent_group->xtttp_stc == NULL) return;

    tcb_t current = ftask;
    if (ime_text_sink) ime_text_sink(msg_type, lData, (uint64_t)WinMpf, ftask);

    if (msg_type == MSG_CHAR)
    {
        xtttp_dtt *terminal = current->parent_group->xtttp_stc;
        char bytes[8]; memcpy(bytes, &lData, sizeof(bytes));
        size_t count = ime_utf8_scalar_bytes(bytes, 4);
        spin_lock(&getch_lock);
        if ((terminal->wait_for_getch || terminal->getch_count) && count && terminal->getch_count + count <= 128)
        {
            for (size_t i = 0; i < count; ++i)
                terminal->getch_bytes[(terminal->getch_head + terminal->getch_count++) % 128] = bytes[i];
            terminal->wait_for_getch = false;
        }
        spin_unlock(&getch_lock);
    }

    // 检查是否有注册的信号处理函数
    if (WinMpf != NULL && ftask->message_pipe_write_fd >= 0)
    {
        MessageInfoFormat mif;
        mif.WinMpf   = WinMpf;
        mif.msg_type = msg_type;
        mif.hData    = hData;
        mif.lData    = lData;

        message_report_failure_once(message_post_window(ftask, mif));
    }
    // 空的就不用管了
}

void do_message(uint64_t msg_type, uint64_t hData, uint64_t lData, MsgPrcor WinMpf, tcb_t ftask)
{
    if (ime_filter_message(msg_type, lData, (uint64_t)WinMpf, ftask)) return;
    do_message_unfiltered(msg_type, hData, lData, WinMpf, ftask);
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

    int64_t worker = static_cast<int64_t>(create_kernel_worker((void *)message_delivery_worker, NULL,
                                                               "GUI Message Delivery"));
    spin_lock(&message_queue_lock);
    message_worker_ready = worker >= 0;
    spin_unlock(&message_queue_lock);
    if (worker < 0) write_serial_fmt("message: delivery worker creation failed: %d\n", worker);
    write_serial_fmt("XJ380 Message Initialize Success.\n");
}

int message_getch_try(xtttp_dtt *terminal)
{
    spin_lock(&getch_lock);
    int result = -1;
    if (terminal->getch_count)
    {
        result = (unsigned char)terminal->getch_bytes[terminal->getch_head];
        terminal->getch_head = (terminal->getch_head + 1) % 128;
        --terminal->getch_count;
    }
    else terminal->wait_for_getch = true;
    spin_unlock(&getch_lock);
    return result;
}
