// SPDX-License-Identifier: Apache-2.0
// Original prepared kernel-group worker publication; no new task ABI fields.
#include <task/kernel_worker.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <user/user.h>
#include <cpu/fpu.h>
#include <lock_queue.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <proto.hpp>
#include <dlinker.h>
#include <errno.h>
#include <fs/vfs/vfs.h>

// Existing kernel-thread return trampoline implemented by pcb.cpp.
void process_exit();
size_t add_task_preallocated(tcb_t task, lock_node *prepared);

size_t create_kernel_worker(void *entry, void *argument, const char *name)
{
    if (!entry || !name || !kernel_group || !kernel_group->thread_queue) return (size_t)-EINVAL;
    if (scheduler_disable_depth()) return (size_t)-EWOULDBLOCK;
    const size_t bytes = (sizeof(struct thread_control_block) + 15ULL) & ~15ULL;
    auto *task = static_cast<tcb_t>(aligned_alloc(16, bytes));
    auto *group_node = static_cast<lock_node *>(malloc(sizeof(lock_node)));
    auto *schedule_node = static_cast<lock_node *>(malloc(sizeof(lock_node)));
    uint64_t stack = task && group_node && schedule_node ? alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE) : 0;
    if (!stack)
    {
        free(task);
        free(group_node);
        free(schedule_node);
        return (size_t)-ENOMEM;
    }
    memset(task, 0, sizeof(*task));
    task->message_pipe_read_fd = task->message_pipe_write_fd = -1;
    task->task_level = TASK_KERNEL_LEVEL;
    task->parent_group = kernel_group;
    task->status = CREATE;
    task->user_info = &root_user;
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->kernel_stack = reinterpret_cast<uintptr_t>(phys_to_virt(stack)) + KERNEL_STACK_SIZE;
    task->context0.rflags = 0x202;
    task->context0.rip = reinterpret_cast<uintptr_t>(entry);
    task->context0.cs = 8;
    task->context0.ss = task->context0.ds = task->context0.es = task->fs = 0x10;
    task->context0.rdi = reinterpret_cast<uintptr_t>(argument);
    task->main = reinterpret_cast<uintptr_t>(entry);
    task->fs_base = reinterpret_cast<uintptr_t>(task);
    task->str_cwd = "/";
    task->cwd = rootdir;
    save_fpu_context(&task->fpu_context);
    auto *top = reinterpret_cast<uint64_t *>(task->kernel_stack);
    *--top = 0;
    *--top = 0;
    *--top = reinterpret_cast<uintptr_t>(process_exit);
    task->context0.rsp = reinterpret_cast<uintptr_t>(top);
    size_t tid = __atomic_load_n(&now_tid, __ATOMIC_ACQUIRE);
    while (tid != size_t(-1) &&
           !__atomic_compare_exchange_n(&now_tid, &tid, tid + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    int error = tid == size_t(-1) ? -EOVERFLOW : 0;
    task->tid = tid;
    bool linked = false;
    if (!error)
    {
        task->group_index = queue_enqueue_preallocated(kernel_group->thread_queue, task, group_node, NULL, NULL);
        linked = task->group_index != size_t(-1);
        if (!linked) error = -EOVERFLOW;
    }
    if (!error && add_task_preallocated(task, schedule_node) == size_t(-1)) error = -EAGAIN;
    if (!error) return tid; // The worker may already have exited; do not read task again.
    if (linked && !queue_detach_preallocated(kernel_group->thread_queue, group_node))
        return (size_t)-EIO; // Preserve uncertain group ownership until reboot.
    free_frames(stack, KERNEL_STACK_SIZE / PAGE_SIZE);
    free(task);
    free(group_node);
    free(schedule_node);
    return (size_t)error;
}
EXPORT_SYMBOL(create_kernel_worker);
