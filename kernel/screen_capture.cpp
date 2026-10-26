#include <screen_capture.h>
#include <cpu/lock.h>
#include <errno.h>
#include <mutex.h>
#include <smp/smp.h>
#include <task/scheduler.h>

namespace
{
spin_t state_lock = SPIN_INIT;
mutex_t capture_gate = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, false};
size_t secret_processes;
size_t mapped_processes;
pcb_t registered_processes;

static pcb_t caller_process()
{
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_APPLICATION_LEVEL || !task->parent_group) return NULL;
    pcb_t process = task->parent_group;
    TaskStatus status = __atomic_load_n(&process->status, __ATOMIC_ACQUIRE);
    if (status == DEATH || status == OUT || status == ZOMBIE) return NULL;
    return process;
}

static bool process_on_cpu(pcb_t process)
{
    if (!process->thread_queue) return false;
    size_t count = get_cpu_num();
    if (count > MAX_CPU_NUM) count = MAX_CPU_NUM;
    bool running = false;
    // Compare pinned thread addresses rather than dereferencing foreign CPU tasks.
    spin_lock(&process->thread_queue->lock);
    for (lock_node *node = process->thread_queue->head; node && !running; node = node->next)
    {
        for (size_t i = 0; i < count; ++i)
        {
            PROCESSOR_INFO *cpu = get_cpu(i);
            if (cpu && __atomic_load_n(&cpu->current_task, __ATOMIC_ACQUIRE) == node->data)
            {
                running = true;
                break;
            }
        }
    }
    spin_unlock(&process->thread_queue->lock);
    return running;
}

static void register_locked(pcb_t process)
{
    if (process->screen_secret || process->screen_raw_mapped) return;
    process->screen_capture_next = registered_processes;
    registered_processes = process;
}

static void release_locked(pcb_t process)
{
    if (process->screen_secret) --secret_processes;
    if (process->screen_raw_mapped) --mapped_processes;
    process->screen_secret = false;
    process->screen_raw_mapped = false;
    process->screen_capture_next = NULL;
}

static void retire_stopped_locked()
{
    // A parent need not waitpid() for capture to resume. The list lock pins PCBs
    // until their exit hook, and the queue lock pins their TCB addresses for the
    // bounded CPU scan. No allocation, sleeping, or rendering occurs here.
    pcb_t *link = &registered_processes;
    while (*link)
    {
        pcb_t process = *link;
        TaskStatus status = __atomic_load_n(&process->status, __ATOMIC_ACQUIRE);
        if ((status == DEATH || status == OUT || status == ZOMBIE) && !process_on_cpu(process))
        {
            *link = process->screen_capture_next;
            release_locked(process);
        }
        else link = &process->screen_capture_next;
    }
}

static bool blocked()
{
    spin_lock(&state_lock);
    retire_stopped_locked();
    bool result = secret_processes != 0;
    spin_unlock(&state_lock);
    return result;
}
} // namespace

int screen_capture_set_secret()
{
    pcb_t process = caller_process();
    if (!process) return -ESRCH;

    // Serialize successful activation with the complete snapshot/delivery operation.
    // This is a recoverable sleeping mutex, never a spin lock or preemption gate.
    int result = mutex_lock_recoverable(&capture_gate);
    if (result < 0) return result;
    spin_lock(&state_lock);
    retire_stopped_locked();
    if (process->screen_secret) result = 0;
    else if (mapped_processes) result = -EBUSY;
    else
    {
        register_locked(process);
        process->screen_secret = true;
        ++secret_processes;
    }
    spin_unlock(&state_lock);
    mutex_unlock(&capture_gate);
    return result;
}

int screen_capture_begin()
{
    if (!caller_process()) return -ESRCH;
    if (blocked()) return -EACCES;
    int result = mutex_lock_recoverable(&capture_gate);
    if (result < 0) return result;
    // Recheck after waiting: SetSecret may have acquired the gate first.
    if (blocked())
    {
        mutex_unlock(&capture_gate);
        return -EACCES;
    }
    return 0;
}

void screen_capture_end()
{
    mutex_unlock(&capture_gate);
}

void screen_capture_register_mapping()
{
    pcb_t process = caller_process();
    if (!process) return;
    // Called while the capture gate excludes SetSecret. Raw mappings remain
    // readable without a syscall; retain the reservation until their process exits.
    spin_lock(&state_lock);
    if (!process->screen_raw_mapped)
    {
        register_locked(process);
        process->screen_raw_mapped = true;
        ++mapped_processes;
    }
    spin_unlock(&state_lock);
}

void screen_capture_process_inherit(pcb_t parent, pcb_t child)
{
    if (!parent || !child) return;
    // Fork calls this before publishing the runnable child. This hook cannot
    // allocate or block: the caller holds its process-publication lock.
    spin_lock(&state_lock);
    if (parent->screen_secret || parent->screen_raw_mapped) register_locked(child);
    if (parent->screen_secret && !child->screen_secret)
    {
        child->screen_secret = true;
        ++secret_processes;
    }
    if (parent->screen_raw_mapped && !child->screen_raw_mapped)
    {
        child->screen_raw_mapped = true;
        ++mapped_processes;
    }
    spin_unlock(&state_lock);
}

void screen_capture_process_exit(pcb_t process)
{
    if (!process) return;
    // Normal teardown has already made all threads terminal. Wait for kernel
    // calls on other CPUs before recovering the mutex and freeing owner TCBs.
    // Unpublished fork-failure objects never run, so their cleanup cannot wait.
    while (process_on_cpu(process)) scheduler_yield();
    mutex_abort_for_process(&capture_gate, process);
    spin_lock(&state_lock);
    pcb_t *link = &registered_processes;
    while (*link && *link != process) link = &(*link)->screen_capture_next;
    if (*link) *link = process->screen_capture_next;
    release_locked(process);
    spin_unlock(&state_lock);
}
