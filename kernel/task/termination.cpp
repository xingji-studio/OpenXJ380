#include <cpu/lock.h>
#include <proto.hpp>
#include <task/scheduler.h>
#include <task/termination.h>

extern bool no_interrupt;

// Lock order: process/thread-list locks before this gate. Never acquire a list
// or run-queue lock, allocate, yield or perform I/O while owning this gate.
static spin_t termination_lock = SPIN_INIT;

static bool terminal(TaskStatus status)
{
    return status == DEATH || status == OUT || status == ZOMBIE;
}

[[noreturn]] static void termination_park()
{
    if (!no_interrupt) enable_intr();
    for (;;)
    {
        scheduler_yield();
        asm volatile("hlt");
    }
}

static void request_task_locked(tcb_t task)
{
    __atomic_store_n(&task->kernel_exit_pending, true, __ATOMIC_RELEASE);
    if (!task->kernel_operation_pinned) __atomic_store_n(&task->status, DEATH, __ATOMIC_RELEASE);
}

void task_request_termination(tcb_t task)
{
    if (!task) return;
    spin_lock(&termination_lock);
    request_task_locked(task);
    spin_unlock(&termination_lock);
}

bool process_request_termination(pcb_t process, int exit_code, bool zombie)
{
    if (!process) return false;
    lock_queue *threads = process->thread_queue;
    if (threads) spin_lock(&threads->lock);
    spin_lock(&termination_lock);
    if (zombie && process->status == ZOMBIE)
    {
        spin_unlock(&termination_lock);
        if (threads) spin_unlock(&threads->lock);
        return false;
    }
    process->exit_code = exit_code;
    if (threads) queue_foreach(threads, node)
        {
            tcb_t task = static_cast<tcb_t>(node->data);
            if (task) request_task_locked(task);
        }
    bool ready = process->kernel_operation_pins == 0;
    if (ready)
    {
        __atomic_store_n(&process->kernel_exit_request, uint8_t(0), __ATOMIC_RELEASE);
        __atomic_store_n(&process->status, DEATH, __ATOMIC_RELEASE);
    }
    else
    {
        // Keep the parent's normal state: scheduler eligibility also depends on
        // that state. A death publication here would strand protected owners.
        __atomic_store_n(&process->kernel_exit_request, uint8_t(zombie ? 1 : 2), __ATOMIC_RELEASE);
        process->kernel_exit_code = exit_code;
    }
    spin_unlock(&termination_lock);
    if (threads) spin_unlock(&threads->lock);
    return ready;
}

bool process_has_kernel_operations(pcb_t process)
{
    return process && __atomic_load_n(&process->kernel_operation_pins, __ATOMIC_ACQUIRE) != 0;
}

bool process_termination_requested(pcb_t process)
{
    return process && __atomic_load_n(&process->kernel_exit_request, __ATOMIC_ACQUIRE) != 0;
}

tcb_t kernel_boundary_enter()
{
    tcb_t task = get_current_task();
    if (!task || !task->parent_group) return NULL;
    spin_lock(&termination_lock);
    ++task->kernel_boundary_depth;
    spin_unlock(&termination_lock);
    return task;
}

tcb_t kernel_operation_enter()
{
    tcb_t task = get_current_task();
    if (!task || !task->parent_group) return NULL;
    spin_lock(&termination_lock);
    pcb_t process  = task->parent_group;
    bool  rejected = terminal(task->status) ||
                    ((!task->kernel_operation_pinned) &&
                     (task->kernel_exit_pending || process->kernel_exit_request || terminal(process->status)));
    if (!rejected)
    {
        if (!task->kernel_operation_pinned)
        {
            task->kernel_operation_pinned = true;
            __atomic_add_fetch(&process->kernel_operation_pins, 1U, __ATOMIC_RELEASE);
        }
        ++task->kernel_operation_depth;
    }
    spin_unlock(&termination_lock);
    if (rejected) termination_park();
    return task;
}

static void finish_scope(tcb_t task, bool boundary)
{
    if (!task) return;
    pcb_t   process = task->parent_group;
    bool    stop = false, finish_process = false;
    int     code    = 0;
    uint8_t request = 0;
    // Keep a terminal current task executing until final process publication;
    // dropping this pin must not let a timer strand its parent notification.
    bool    interrupts = are_interrupts_enabled();
    disable_intr();
    spin_lock(&termination_lock);
    if (boundary)
    {
        if (task->kernel_boundary_depth) --task->kernel_boundary_depth;
    }
    else if (task->kernel_operation_depth)
        --task->kernel_operation_depth;
    bool unwinding = task->kernel_exit_pending && task->kernel_boundary_depth != 0;
    if (task->kernel_operation_pinned && !task->kernel_operation_depth && !unwinding)
    {
        task->kernel_operation_pinned = false;
        __atomic_sub_fetch(&process->kernel_operation_pins, 1U, __ATOMIC_RELEASE);
    }
    if (!task->kernel_operation_depth && !task->kernel_boundary_depth && task->kernel_exit_pending)
    {
        __atomic_store_n(&task->status, DEATH, __ATOMIC_RELEASE);
        stop           = true;
        request        = process->kernel_exit_request;
        code           = process->kernel_exit_code;
        finish_process = request && process->kernel_operation_pins == 0;
    }
    spin_unlock(&termination_lock);
    // The current CPU still pins this task/PCB. Parent notification and process
    // collection must happen outside metadata locks, after every scope unwound.
    if (finish_process) kill_proc(process, code, request == 1);
    if (stop) termination_park();
    if (interrupts && !no_interrupt) enable_intr();
}

void kernel_operation_leave(tcb_t task)
{
    finish_scope(task, false);
}

void kernel_boundary_leave(tcb_t task)
{
    finish_scope(task, true);
}
