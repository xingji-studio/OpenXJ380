#include "task/pcb.h"
#include "cpu/fsgsbase.h"
#include "cpu/regio.h"
#include "krlibc.h"
#include "mm/alloc/alloc.h"
#include "mm/heap.h"
#include "mm/page.h"
#include "proto.hpp"
#include <dlinker.h>
#include <cpu/longm.h>
#include <elf.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <mm/lazyalloc.h>
#include <mm/uaccess.h>
#include <pctable/gdt.h>
#include <procfs.h>
#include <syscall/syscall.h>
#include <task/ipc.h>
#include <user_image_candidate.h>
#include <apic/apic.h>

#define SA_RPL3 3

#define SA_RPL_MASK      0xFFFC
#define SA_TI_MASK       0xFFFB
#define GET_SEL(cs, rpl) ((cs & SA_RPL_MASK & SA_TI_MASK) | (rpl))

#define PUSH_STACK(stack, value) *(--stack) = value
static size_t now_pid = 0;
size_t now_tid = 0;
static constexpr size_t EXECVE_STRING_MAX       = 4096;
static constexpr size_t EXECVE_VECTOR_MAX       = 128;
static constexpr size_t EXECVE_TOTAL_STRING_MAX = PAGE_SIZE * 4;
lock_queue   *pcb_group_queue = NULL;
pcb_t         kernel_group    = NULL;

uint64_t task_user_fs_selector(tcb_t task)
{
    return task != NULL ? task->fs : 0;
}

static size_t alloc_pid()
{
    return __atomic_fetch_add(&now_pid, 1, __ATOMIC_SEQ_CST);
}

static size_t alloc_tid()
{
    return __atomic_fetch_add(&now_tid, 1, __ATOMIC_SEQ_CST);
}

static void reserve_tid_value(size_t tid)
{
    size_t next = __atomic_load_n(&now_tid, __ATOMIC_SEQ_CST);
    while (next <= tid &&
           !__atomic_compare_exchange_n(&now_tid, &next, tid + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
    }
}

extern UserInfo *current_user;

extern UserInfo root_user;

extern bool no_interrupt;
extern bool is_scheduler;
spin_t create_thread_lock = SPIN_INIT;
static spin_t execve_image_lock = SPIN_INIT;

static void finish_execve_transition(pcb_t process)
{
    spin_lock(&create_thread_lock);
    process->exec_in_progress = false;
    spin_unlock(&create_thread_lock);
}

extern "C" XSK_SMP_INFO *xsi;
extern uint64_t          lapic_id();
extern bool              lapic_send_fixed_ipi(uint32_t destination_lapic_id, uint8_t vector);

#define LAPIC_RESCHEDULE_VECTOR 0xf1

static bool build_exec_freeze_target_mask(pcb_t process, uint64_t *target_mask)
{
    if (process == NULL || target_mask == NULL || xsi == NULL) return false;
    memset(target_mask, 0, sizeof(uint64_t) * EXEC_FREEZE_CPU_WORDS);
    size_t cpu_count = xsi->cpu_count;
    if (cpu_count > MAX_CPU_NUM) cpu_count = MAX_CPU_NUM;
    for (size_t i = 0; i < cpu_count; i++)
    {
        PROCESSOR_INFO *cpu = &xsi->pcr_inf[i];
        if (cpu == NULL) continue;
        tcb_t running = cpu->current_task;
        if (running == NULL || running->parent_group != process) continue;
        if (running == get_current_task()) continue;
        size_t word = i / 64;
        size_t bit  = i % 64;
        target_mask[word] |= (1ULL << bit);
    }
    return true;
}

static bool freeze_process_for_exec(pcb_t process)
{
    if (process == NULL || xsi == NULL) return true;
    uint64_t target_mask[EXEC_FREEZE_CPU_WORDS];
    if (!build_exec_freeze_target_mask(process, target_mask)) return true;

    bool any_target = false;
    for (size_t i = 0; i < EXEC_FREEZE_CPU_WORDS; i++)
    {
        if (target_mask[i] != 0)
        {
            any_target = true;
            break;
        }
    }
    if (!any_target)
    {
        process->exec_freeze_state         = EXEC_FREEZE_RUNNING;
        process->exec_freeze_generation    = 0;
        memset(process->exec_freeze_ack_mask, 0, sizeof(process->exec_freeze_ack_mask));
        write_serial_fmt("execve: freeze ack complete pid=%llu gen=0 targets=0 (noop)\n",
                         (unsigned long long)process->pid);
        return true;
    }

    spin_lock(&create_thread_lock);
    process->exec_freeze_generation++;
    uint64_t generation = process->exec_freeze_generation;
    for (size_t i = 0; i < EXEC_FREEZE_CPU_WORDS; i++) process->exec_freeze_target_mask[i] = target_mask[i];
    memset(process->exec_freeze_ack_mask, 0, sizeof(process->exec_freeze_ack_mask));
    process->exec_freeze_state = EXEC_FREEZE_REQUESTED;
    spin_unlock(&create_thread_lock);

    size_t cpu_count = xsi->cpu_count;
    if (cpu_count > MAX_CPU_NUM) cpu_count = MAX_CPU_NUM;
    for (size_t i = 0; i < cpu_count; i++)
    {
        size_t word = i / 64;
        size_t bit  = i % 64;
        if ((target_mask[word] & (1ULL << bit)) == 0) continue;
        PROCESSOR_INFO *cpu = &xsi->pcr_inf[i];
        if (cpu == NULL) continue;
        lapic_send_fixed_ipi((uint32_t)cpu->lapic_id, LAPIC_RESCHEDULE_VECTOR);
    }

    /* Spin until each target CPU acknowledges that it stopped using the old
     * image. The scheduler writes the ack bit from the timer path, which is
     * triggered by the reschedule IPI. We yield locally so the BSP also
     * advances a tick if it is itself a target. */
    const uint64_t deadline_ns = nanoTime() + 500000000ULL;
    while (nanoTime() < deadline_ns)
    {
        bool ready = true;
        for (size_t i = 0; i < EXEC_FREEZE_CPU_WORDS; i++)
        {
            uint64_t expected = target_mask[i];
            uint64_t observed = __atomic_load_n(&process->exec_freeze_ack_mask[i], __ATOMIC_ACQUIRE);
            if ((observed & expected) != expected)
            {
                ready = false;
                break;
            }
        }
        if (ready) break;
        if (process->exec_freeze_generation != generation)
        {
            return false;
        }
        scheduler_yield();
    }

    for (size_t i = 0; i < EXEC_FREEZE_CPU_WORDS; i++)
    {
        uint64_t expected = target_mask[i];
        uint64_t observed = __atomic_load_n(&process->exec_freeze_ack_mask[i], __ATOMIC_ACQUIRE);
        if ((observed & expected) != expected)
        {
            write_serial_fmt("execve: freeze ack timeout pid=%llu gen=%llu\n",
                             (unsigned long long)process->pid,
                             (unsigned long long)generation);
            return false;
        }
    }
    spin_lock(&create_thread_lock);
    if (process->exec_freeze_state == EXEC_FREEZE_REQUESTED) process->exec_freeze_state = EXEC_FREEZE_FROZEN;
    spin_unlock(&create_thread_lock);
    write_serial_fmt("execve: freeze ack complete pid=%llu gen=%llu targets=%llx\n",
                     (unsigned long long)process->pid,
                     (unsigned long long)generation,
                     (unsigned long long)target_mask[0]);
    return true;
}

static void unfreeze_process(pcb_t process)
{
    if (process == NULL) return;
    spin_lock(&create_thread_lock);
    process->exec_freeze_state = EXEC_FREEZE_RUNNING;
    spin_unlock(&create_thread_lock);
}

static bool process_freeze_targets_acked(pcb_t process)
{
    if (process == NULL) return true;
    for (size_t i = 0; i < EXEC_FREEZE_CPU_WORDS; i++)
    {
        uint64_t expected = process->exec_freeze_target_mask[i];
        uint64_t observed = __atomic_load_n(&process->exec_freeze_ack_mask[i], __ATOMIC_ACQUIRE);
        if ((observed & expected) != expected) return false;
    }
    return true;
}
static spin_t user_stack_build_lock = SPIN_INIT;
static user_image_retirement_queue_t execve_retirement_queue;
// pcb.cpp
#include "task/scheduler.h"

extern spin_t scheduler_lock;
extern void message_thread(uint64_t arg);
extern uint64_t message_ask(uint64_t msg_type_p, uint64_t hdatap, uint64_t ldatap, uint64_t funcp, uint64_t taskp);
static void close_exec_file_descriptors(pcb_t process);
static void close_process_file_table(pcb_t pcb);
static bool fd_table_unref(pcb_t pcb);

static tcb_t alloc_zeroed_tcb(void)
{
    static_assert(offsetof(struct thread_control_block, syscall_stack) == 0xb48,
                  "Update kernel/intr/handler.S THREAD_SYSCALL_STACK when changing TCB layout");
    static_assert(offsetof(struct thread_control_block, syscall_user_rsp) == 0xb50,
                  "Update kernel/intr/handler.S THREAD_SYSCALL_USER_RSP when changing TCB layout");

    const size_t alloc_size = (sizeof(struct thread_control_block) + 15ULL) & ~15ULL;
    tcb_t        task = (tcb_t)aligned_alloc(16, alloc_size);
    if (task != NULL)
    {
        memset(task, 0, sizeof(struct thread_control_block));
        task->message_pipe_read_fd  = -1;
        task->message_pipe_write_fd = -1;
    }
    return task;
}

uint16_t dock_ct_sheet;
static constexpr uint64_t MESSAGE_THREAD_STACK_HEADROOM = PAGE_SIZE;

static bool ensure_message_entry_stub(page_directory_t *pagedir)
{
    if (pagedir == NULL) return false;

    constexpr uint64_t stub_page = XJ380_PRIVATE_MESSAGE_REVERT_ADDRESS;
    const size_t message_thread_size = (size_t)((uintptr_t)message_ask - (uintptr_t)message_thread);
    if (message_thread_size == 0 || message_thread_size > (PAGE_SIZE - XPSR_OFFEST)) return false;

    if (translate_address(pagedir, stub_page) == 0)
    {
        page_map_range_to_random(pagedir, stub_page, PAGE_SIZE, PTE_PRESENT | PTE_WRITEABLE | PTE_USER);
    }

    page_directory_t *current_dir = get_current_directory();
    switch_page_directory(pagedir);
    memcpy((void *)(stub_page + XPSR_OFFEST), (void *)message_thread, message_thread_size);
    switch_page_directory(current_dir);
    return true;
}

static void restore_runtime_state(bool was_scheduler_enabled, bool was_interrupt_enabled)
{
    if (was_scheduler_enabled) enable_scheduler();
    if (was_interrupt_enabled && !no_interrupt) open_interrupt;
    else close_interrupt;
}

static bool append_cmdline_char(char *cmdline, size_t cmdline_size, char **cursor, char ch)
{
    if (cmdline == NULL || cursor == NULL || *cursor == NULL) return false;

    size_t used = (size_t)(*cursor - cmdline);
    if (used + 1 >= cmdline_size) return false;

    **cursor = ch;
    (*cursor)++;
    **cursor = '\0';
    return true;
}

static bool append_cmdline_arg(char *cmdline, size_t cmdline_size, char **cursor, const char *arg)
{
    if (cmdline == NULL || cursor == NULL || *cursor == NULL || arg == NULL) return false;

    bool quote = arg[0] == '\0';
    for (const char *p = arg; *p != '\0'; p++)
    {
        if (*p == ' ' || *p == '\'' || *p == '"' || *p == '\\')
        {
            quote = true;
            break;
        }
    }

    if (quote && !append_cmdline_char(cmdline, cmdline_size, cursor, '"')) return false;
    for (const char *p = arg; *p != '\0'; p++)
    {
        if (quote && (*p == '"' || *p == '\\') &&
            !append_cmdline_char(cmdline, cmdline_size, cursor, '\\'))
        {
            return false;
        }
        if (!append_cmdline_char(cmdline, cmdline_size, cursor, *p)) return false;
    }
    if (quote && !append_cmdline_char(cmdline, cmdline_size, cursor, '"')) return false;
    if (!append_cmdline_char(cmdline, cmdline_size, cursor, ' ')) return false;
    return true;
}

static void invalidate_process_message_pipes(pcb_t process)
{
    if (process == NULL || process->thread_queue == NULL) return;

    spin_lock(&process->thread_queue->lock);
    queue_foreach(process->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread == NULL) continue;
        thread->message_pipe_read_fd  = -1;
        thread->message_pipe_write_fd = -1;
    }
    spin_unlock(&process->thread_queue->lock);
}

pcb_t found_pcb(int pid)
{
    if (pcb_group_queue == NULL) return NULL;
    pcb_t ret = NULL;
    spin_lock(&pcb_group_queue->lock);
    queue_foreach(pcb_group_queue, node)
    {
        pcb_t pcb = (pcb_t)node->data;
        if (pcb->pid == pid)
        {
            ret = pcb;
            break;
        }
    }
    spin_unlock(&pcb_group_queue->lock);
    return ret;
}

pcb_t found_process_by_exe_path(const char *exe_path)
{
    if (pcb_group_queue == NULL || exe_path == NULL || exe_path[0] == '\0') return NULL;

    pcb_t ret = NULL;
    spin_lock(&pcb_group_queue->lock);
    queue_foreach(pcb_group_queue, node)
    {
        pcb_t pcb = (pcb_t)node->data;
        if (pcb == NULL || pcb->exe_path == NULL) continue;
        if (pcb->status == DEATH || pcb->status == OUT || pcb->status == ZOMBIE) continue;
        if (strcmp(pcb->exe_path, exe_path) == 0)
        {
            ret = pcb;
            break;
        }
    }
    spin_unlock(&pcb_group_queue->lock);
    return ret;
}

tcb_t found_thread(pcb_t pcb, int tid)
{
    if (pcb == NULL || pcb->thread_queue == NULL) return NULL;
    tcb_t ret = NULL;
    spin_lock(&pcb->thread_queue->lock);
    queue_foreach(pcb->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread != NULL && thread->tid == tid)
        {
            ret = thread;
            break;
        }
    }
    spin_unlock(&pcb->thread_queue->lock);
    return ret;
}

void kill_proc(pcb_t pcb, int exit_code, bool is_zombie)
{
    if (pcb == NULL) return;
    if (pcb->pid == kernel_group->pid)
    {
        write_serial_fmt("Cannot kill System process.");
        return;
    }

    pcb->exit_code = exit_code;

    if (is_zombie)
    {
        close_process_file_table(pcb);
        pcb->status = ZOMBIE;
        if (pcb->thread_queue->size > 0)
        {
            spin_lock(&pcb->thread_queue->lock);
            queue_foreach(pcb->thread_queue, node)
            {
                tcb_t tcb = (tcb_t)node->data;
                kill_thread(tcb);
            }
            spin_unlock(&pcb->thread_queue->lock);
        }
        ipc_message_t msg = (ipc_message_t)malloc(sizeof(struct ipc_message));
        if (msg == NULL)
        {
            write_serial_fmt("task: cannot notify parent of process %s exit.\n", pcb->name);
            return;
        }
        msg->pid          = pcb->pid;
        msg->type         = IPC_MSG_TYPE_EPID;
        msg->data[0]      = exit_code & 0xFF;
        msg->data[1]      = (exit_code >> 8) & 0xFF;
        msg->data[2]      = (exit_code >> 16) & 0xFF;
        msg->data[3]      = (exit_code >> 24) & 0xFF;
        ipc_send(pcb->parent_task, msg);
    }
    else
    {
        if (pcb->parent_task != NULL && pcb->parent_task->child_pcb != NULL)
            queue_remove_data(pcb->parent_task->child_pcb, pcb);
        pcb->status = DEATH;
        kill_proc0(pcb);
    }
}

bool kill_proc_deferred(pcb_t pcb, int exit_code)
{
    if (pcb == NULL || kernel_group == NULL || pcb->pid == kernel_group->pid) return false;

    if (pcb->parent_task != kernel_group)
    {
        size_t reaper_index = queue_enqueue(kernel_group->child_pcb, pcb);
        if (reaper_index == (size_t)-1) return false;

        if (pcb->parent_task != NULL && pcb->parent_task->child_pcb != NULL)
            queue_remove_data(pcb->parent_task->child_pcb, pcb);
        pcb->parent_task = kernel_group;
        pcb->ppid = kernel_group->pid;
        pcb->child_index = reaper_index;
    }

    pcb->exit_code = exit_code;
    close_process_file_table(pcb);
    pcb->status = DEATH;

    if (pcb->thread_queue != NULL)
    {
        spin_lock(&pcb->thread_queue->lock);
        queue_foreach(pcb->thread_queue, node)
        {
            tcb_t tcb = (tcb_t)node->data;
            kill_thread(tcb);
        }
        spin_unlock(&pcb->thread_queue->lock);
    }

    return true;
}

void free_envp(char **envp)
{
    if (!envp) return;
    for (size_t i = 0; envp[i] != NULL; i++)
    {
        free(envp[i]);
    }
    free(envp);
}

static char **copy_kernel_string_vector_owned(char **argv, size_t argc)
{
    if (argv == NULL || argc == 0) return NULL;
    char **copy = (char **)calloc(argc + 1, sizeof(char *));
    if (copy == NULL) return NULL;
    for (size_t i = 0; i < argc; i++)
    {
        if (argv[i] == NULL)
        {
            free_envp(copy);
            return NULL;
        }
        copy[i] = strdup(argv[i]);
        if (copy[i] == NULL)
        {
            free_envp(copy);
            return NULL;
        }
    }
    copy[argc] = NULL;
    return copy;
}

void kill_proc0(pcb_t pcb)
{
    procfs_on_exit_task(pcb);
    spin_lock(&pcb->thread_queue->lock);
    queue_foreach(pcb->thread_queue, thread_node)
    {
        tcb_t thread = (tcb_t)thread_node->data;
        if (thread != NULL) thread->status = DEATH;
    }
    spin_unlock(&pcb->thread_queue->lock);

    close_process_file_table(pcb);

    do
    {
        tcb_t thread = (tcb_t)queue_dequeue(pcb->thread_queue);
        if (thread == NULL) break;
        kill_thread0(thread);
        free(thread);
    } while (true);

    queue_destroy(pcb->thread_queue);
    queue_remove_data(pcb_group_queue, pcb);

    // 保存需要在free之后访问的字段
    char *cmdline = pcb->cmdline;
    char **argv = pcb->argv;
    char *exe_path = pcb->exe_path;
    void *elf_file = pcb->elf_file;
    char **envp = pcb->envp;
    tty_t *tty = pcb->tty;
    xtttp_dtt *xtttp = pcb->xtttp_stc;
    char name[sizeof(pcb->name)];
    memset(name, 0, sizeof(name));
    strncpy(name, pcb->name, sizeof(name) - 1);
    int pid = pcb->pid;
    bool vfork = pcb->vfork;

    // 先销毁队列，因为它们需要访问pcb中的指针
    lock_queue *ipc_queue = pcb->ipc_queue;
    lock_queue *virt_queue = pcb->virt_queue;
    page_directory_t *pagedir = pcb->pagedir;

    lazy_free(pcb);

    pcb->file_open = NULL;
    pcb->ipc_queue = NULL;
    pcb->virt_queue = NULL;

    // 现在销毁队列
    if (ipc_queue) queue_destroy(ipc_queue);
    if (virt_queue) queue_destroy(virt_queue);

    free(cmdline);
    if (argv) free_envp(argv);
    free(exe_path);
    free(elf_file);
    if (envp) free_envp(envp);
    if (!vfork && pagedir != NULL && pagedir != get_kernel_pagedir()) free_page_directory(pagedir);
    free_tty(tty);
    free(xtttp);
    free(pcb);
    write_serial_fmt("task: Freeing process %s (PID: %d) vfork: %s\n", name, pid,
                     vfork ? "true" : "false");
}

static void clear_child_tid_if_requested(tcb_t task)
{
    if (task == NULL || task->clear_child_tid == 0 || task->tid_directory == NULL) return;

    int zero = 0;
    copy_to_user_pagedir(task->tid_directory, (void *)task->clear_child_tid, &zero, sizeof(zero));
    task->clear_child_tid = 0;
    task->tid_directory = NULL;
}

static void free_user_stack_mapping(tcb_t task)
{
    if (task == NULL || task->parent_group == NULL || task->parent_group->pagedir == NULL ||
        task->user_stack == 0 || task->user_stack_top <= task->user_stack)
        return;

    unmap_virtual_page(task->parent_group, task->user_stack, task->user_stack_top - task->user_stack);
    unmap_page_range(task->parent_group->pagedir, task->user_stack, task->user_stack_top - task->user_stack);
    task->user_stack = 0;
    task->user_stack_top = 0;
}

void kill_thread(tcb_t task)
{
    if (task == NULL) return;
    if (task->task_level == TASK_IDLE_LEVEL)
    {
        write_serial_fmt("Cannot stop kernel thread.");
        return;
    }
    clear_child_tid_if_requested(task);
    task->status = DEATH;
    // kill_thread0(task); 瑕佺敤鐨勮В鎺?浣嗗彲鑳借鍔犻攣
}

extern void remove_task(tcb_t task);

void kill_thread0(tcb_t task)
{
    task->status = OUT;
    clear_child_tid_if_requested(task);
    if (task->argv != NULL)
    {
        for (size_t i = 0; task->argv[i] != NULL; i++)
            free(task->argv[i]);
        free(task->argv);
        task->argv = NULL;
    }
    if (task->task_level == TASK_APPLICATION_LEVEL && task->cwd != NULL)
    {
        vfs_close(task->cwd);
        task->cwd = NULL;
    }
    if (task->task_level == TASK_APPLICATION_LEVEL && task->str_cwd != NULL)
    {
        free(task->str_cwd);
        task->str_cwd = NULL;
    }
    if (task->task_level == TASK_APPLICATION_LEVEL && task->owns_user_stack && task->parent_group != NULL &&
        task->parent_group->pagedir != NULL && task->user_stack != 0)
    {
        free_user_stack_mapping(task);
    }
    free_frames((uint64_t)virt_to_phys((task->kernel_stack - KERNEL_STACK_SIZE)), KERNEL_STACK_SIZE / PAGE_SIZE);
    if (task->syscall_stack != 0)
        free_frames((uint64_t)virt_to_phys((task->syscall_stack - KERNEL_STACK_SIZE)), KERNEL_STACK_SIZE / PAGE_SIZE);
    remove_task(task);
}

[[noreturn]] void process_exit()
{
    uint64_t rax = 0;
    __asm__("movq %%rax,%0" ::"r"(rax) :);
    write_serial_string("Kernel thread exit, Code: ");
    write_serial_dec(rax);
    write_serial_string("\n");
    kill_thread(get_current_task());
    open_interrupt;
    while (true)
        __asm__ volatile("hlt");
}

static uint64_t push_slice(tcb_t task, uint64_t ustack, const uint8_t *slice, uint64_t len)
{
    if (task == NULL || task->parent_group == NULL || task->parent_group->pagedir == NULL) return 0;

    uint64_t tmp_stack = ustack;
    tmp_stack -= len;
    tmp_stack -= (tmp_stack % 0x08);
    if (!copy_to_user_pagedir(task->parent_group->pagedir, (void *)tmp_stack, slice, len)) return 0;
    return tmp_stack;
}

static void fill_aux_random(uint8_t random_bytes[16], tcb_t task, uint64_t sp)
{
    uint64_t seed = nanoTime() ^ (uint64_t)(uintptr_t)task ^ (sp << 7);

    for (size_t i = 0; i < 16; i++)
    {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        seed *= 0x2545f4914f6cdd1dULL;
        random_bytes[i] = (uint8_t)(seed >> ((i & 7U) * 8));
    }
}

static uint64_t push_auxv(tcb_t task, uint64_t tmp_stack, uint64_t *tmp, uint64_t type, uint64_t value)
{
    tmp[0] = type;
    tmp[1] = value;
    return push_slice(task, tmp_stack, (const uint8_t *)tmp, 2 * sizeof(uint64_t));
}

static uint64_t build_user_stack(tcb_t task, uint64_t sp, uint64_t entry_point, uint64_t link_start, uint8_t *link_data,
                                 size_t link_size, uint64_t *ep)
{
    uint64_t env_i  = 0;
    int      argv_i = 0;
    bool     ok     = true;

    char *parsed_argv[EXECVE_VECTOR_MAX + 1];
    char **argv = task->argv;
    int argc = (int)task->argc;
    bool argv_owned_by_parser = false;
    char *build_cmdline = NULL;
    if (argv == NULL || argc <= 0)
    {
        build_cmdline = strdup(task->parent_group->cmdline);
        argc = cmd_parse_limit(build_cmdline, parsed_argv, ' ', EXECVE_VECTOR_MAX);
        argv = parsed_argv;
        argv_owned_by_parser = true;
    }
    if (argc < 0)
    {
        if (build_cmdline != NULL) free(build_cmdline);
        return 0;
    }
    if (argc > (int)EXECVE_VECTOR_MAX) argc = (int)EXECVE_VECTOR_MAX;
    write_serial_fmt("[busybox-debug] build_user_stack proc=%s pid=%llu argc=%d argv_source=%s cmdline=%s\n",
                     task->parent_group ? task->parent_group->name : "(null)",
                     task->parent_group ? (unsigned long long)task->parent_group->pid : 0ULL,
                     argc,
                     argv_owned_by_parser ? "cmdline" : "tcb",
                     task->parent_group && task->parent_group->cmdline ? task->parent_group->cmdline : "(null)");
    if (argv != NULL)
    {
        for (int i = 0; i < argc && argv[i] != NULL; i++)
        {
            write_serial_fmt("[busybox-debug] build_user_stack argv[%d]=%s\n", i, argv[i]);
        }
    }

    char **envp = task->parent_group->envp ? task->parent_group->envp : current_user->envp;

    uint64_t tmp_stack = sp;
    tmp_stack          = push_slice(task, tmp_stack, (const uint8_t *)task->name, strlen(task->name) + 1);
    ok                 = tmp_stack != 0;
    if (ok) task->parent_group->aux_execfn = tmp_stack;
    static const char platform[] = "x86_64";
    if (ok) tmp_stack = push_slice(task, tmp_stack, (const uint8_t *)platform, sizeof(platform));
    ok = ok && tmp_stack != 0;
    uint64_t aux_platform_ptr = tmp_stack;

    size_t env_limit = 0;
    if (envp != NULL) env_limit = task->parent_group->envp ? task->parent_group->envc : current_user->envc;

    uint64_t *envps = (uint64_t *)calloc(env_limit + 1, sizeof(uint64_t));
    uint64_t *argvps = (uint64_t *)calloc((size_t)argc + 1, sizeof(uint64_t));
    if (envps == NULL || argvps == NULL)
    {
        free(envps);
        free(argvps);
        free(link_data);
        if (argv_owned_by_parser)
        {
            cmd_free(argv, argc);
            free(build_cmdline);
        }
        return 0;
    }

    if (envp != NULL)
    {
        for (size_t i = 0; ok && i < env_limit; i++)
        {
            if (envp[i] != NULL)
            {
                tmp_stack       = push_slice(task, tmp_stack, (const uint8_t *)envp[i], strlen(envp[i]) + 1);
                ok              = tmp_stack != 0;
                envps[env_i++] = tmp_stack;
            }
        }
    }

    for (argv_i = 0; ok && argv_i < argc; argv_i++)
    {
        tmp_stack      = push_slice(task, tmp_stack, (const uint8_t *)argv[argv_i], strlen(argv[argv_i]) + 1);
        ok             = tmp_stack != 0;
        argvps[argv_i] = tmp_stack;
    }
    if (ok && argv_i > 0) task->parent_group->aux_execfn = argvps[0];

    uint8_t aux_random[16];
    fill_aux_random(aux_random, task, sp);
    if (ok) tmp_stack = push_slice(task, tmp_stack, aux_random, sizeof(aux_random));
    ok = ok && tmp_stack != 0;
    uint64_t aux_random_ptr = tmp_stack;

    uint64_t total_length = 2 * sizeof(uint64_t) + 24 * 2 * sizeof(uint64_t) + (env_i + 0) * sizeof(uint64_t) +
                            sizeof(uint64_t) + (argv_i + 0) * sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t);
    tmp_stack -= (tmp_stack - total_length) % 0x10;

    // push auxv
    uint8_t *tmp = (uint8_t *)malloc(2 * sizeof(uint64_t));
    if (tmp == NULL)
    {
        free(envps);
        free(argvps);
        free(link_data);
        if (argv_owned_by_parser)
        {
            cmd_free(argv, argc);
            free(build_cmdline);
        }
        return 0;
    }
    memset(tmp, 0, 2 * sizeof(uint64_t));
    uint64_t *aux_tmp = (uint64_t *)tmp;
    uint64_t uid = user_uid(task->user_info);
    uint64_t gid = user_gid(task->user_info);
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_NULL, 0);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_SECURE, 0);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_EXECFN, task->parent_group->aux_execfn);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_CLKTCK, 100);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_PLATFORM, aux_platform_ptr);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_HWCAP, 0);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_EGID, gid);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_GID, gid);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_EUID, uid);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_UID, uid);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_FLAGS, 0);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_BASE, task->parent_group->aux_base);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_RANDOM, aux_random_ptr);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_ENTRY,
                                  task->parent_group->aux_entry ? task->parent_group->aux_entry : entry_point);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_PHNUM, task->parent_group->aux_phnum);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_PHENT, task->parent_group->aux_phent);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_PHDR, task->parent_group->aux_phdr);
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_auxv(task, tmp_stack, aux_tmp, AT_PAGESZ, PAGE_SIZE);
    ok = ok && tmp_stack != 0;

    memset(aux_tmp, 0, 2 * sizeof(uint64_t));

    if (ok) tmp_stack = push_slice(task, tmp_stack, tmp, sizeof(uint64_t));
    ok = ok && tmp_stack != 0;

    if (ok) tmp_stack = push_slice(task, tmp_stack, (const uint8_t *)envps, env_i * sizeof(uint64_t));
    ok = ok && tmp_stack != 0;
    *ep = tmp_stack;

    if (ok) tmp_stack = push_slice(task, tmp_stack, tmp, sizeof(uint64_t));
    ok = ok && tmp_stack != 0;
    if (ok) tmp_stack = push_slice(task, tmp_stack, (const uint8_t *)argvps, argv_i * sizeof(uint64_t));
    ok = ok && tmp_stack != 0;

    uint64_t argc_value = (uint64_t)argv_i;
    if (ok) tmp_stack = push_slice(task, tmp_stack, (const uint8_t *)&argc_value, sizeof(argc_value));
    ok = ok && tmp_stack != 0;

    free(tmp);
    free(envps);
    free(argvps);
    free(link_data);
    if (argv_owned_by_parser)
    {
        cmd_free(argv, argc);
        free(build_cmdline);
    }

    return ok ? tmp_stack : 0;
}

static void switch_task_to_user_mode(tcb_t task)
{
    close_interrupt;

    if (task == NULL || task->parent_group == NULL)
    {
        write_serial_string("switch_to_user_mode: invalid current task\n");
        process_exit();
    }

    uint64_t rsp = task->uses_prepared_user_stack ? task->prepared_user_rsp : task->user_stack_top;
    page_directory_t *current_dir = get_current_directory();
    page_directory_t *target_dir  = task->parent_group->pagedir;
    if (target_dir != NULL && current_dir != target_dir)
    {
        switch_page_directory(target_dir);
    }

    uint64_t ep = task->prepared_user_envp;
    if (!task->uses_prepared_user_stack)
    {
        spin_lock(&user_stack_build_lock);
        rsp = build_user_stack(task, rsp, task->main, 0, NULL, 0, &ep);
        spin_unlock(&user_stack_build_lock);
    }
    if (rsp == 0)
    {
        write_serial_string("build_user_stack failed\n");
        process_exit();
    }

    uint64_t argc = task->uses_prepared_user_stack ? task->argc : 0;
    if (!task->uses_prepared_user_stack &&
        !copy_from_user_pagedir(task->parent_group->pagedir, &argc, (const void *)rsp, sizeof(argc)))
    {
        write_serial_string("user stack argc fetch failed\n");
        process_exit();
    }
    char **argv = task->uses_prepared_user_stack ? (char **)task->prepared_user_argv
                                                 : (char **)(rsp + sizeof(uint64_t));
    uint64_t entry_rdx = task->uses_prepared_user_stack ? task->prepared_user_entry_rdx : ep;

    task->context0.rflags = 0x202;
    task->context0.rsp    = rsp;
    task->context0.rdi    = argc;
    task->context0.rsi    = (uint64_t)argv;
    task->context0.rdx    = entry_rdx;
    uint64_t func         = task->main;

    bool need_stdio = task->parent_group->file_open == NULL || queue_get(task->parent_group->file_open, 0) == NULL;
    if (need_stdio)
    {
        char *new_path = (char *)malloc(strlen("/dev/") + 10);
        sprintf(new_path, "/dev/stdio");
        vfs_node_t stdio        = vfs_open(new_path);
        if (stdio != NULL)
        {
            stdio->refcount       += 3;
            fd_file_handle *stdout = (fd_file_handle *)calloc(1, sizeof(fd_file_handle));
            stdout->node           = stdio;
            stdout->offset         = 0;
            fd_file_handle *stdin  = (fd_file_handle *)calloc(1, sizeof(fd_file_handle));
            stdin->node            = stdio;
            stdin->offset          = 0;
            fd_file_handle *stderr = (fd_file_handle *)calloc(1, sizeof(fd_file_handle));
            stderr->node           = stdio;
            stderr->offset         = 0;
            stdin->fd              = queue_enqueue(task->parent_group->file_open, stdin);
            stdout->fd             = queue_enqueue(task->parent_group->file_open, stdout);
            stderr->fd             = queue_enqueue(task->parent_group->file_open, stderr);
        }
        else
        {
            write_serial_string("Warning: /dev/stdio not available for user task bootstrap.\n");
        }
        free(new_path);
    }

    write_kgsbase((uint64_t)get_current_cpu());
    write_gsbase(0);
    uint64_t fs_selector = task_user_fs_selector(task);
    __asm__ __volatile__("movq %0, %%fs\n\t" : : "r"(fs_selector));
    write_fsbase(task->fs_base);

    __asm__ volatile("mov %0, %%es\n"
                     "mov %0, %%ds\n"
                     "pushq %5\n"
                     "pushq %1\n"
                     "pushq %2\n"
                     "pushq %3\n"
                     "pushq %4\n"
                     "mov %6, %%rdi\n"
                     "mov %7, %%rsi\n"
                     "mov %8, %%rdx\n"
                     "iretq\n"
                     :
                     : "r"((uint64_t)SELECTOR_USER_DS), "r"(rsp), "r"(task->context0.rflags),
                       "r"((uint64_t)SELECTOR_USER_CS), "r"(func), "r"((uint64_t)SELECTOR_USER_DS),
                       "r"(argc),
                       "r"((uint64_t)argv),
                       "r"(entry_rdx)
                     : "memory");
}

void switch_to_user_mode()
{
    switch_task_to_user_mode(get_current_task());
}



void jump_to_message(uint64_t tcb)
{
    close_interrupt;
    uint64_t rsp_value = get_current_task()->user_stack_top;
    if (rsp_value > MESSAGE_THREAD_STACK_HEADROOM) rsp_value -= MESSAGE_THREAD_STACK_HEADROOM;
    rsp_value &= ~0xFULL;
    rsp_value -= sizeof(uint64_t);
    *(uint64_t *)rsp_value = 0;
    uint64_t *rsp = (uint64_t *)rsp_value;

    get_current_task()->context0.rflags = 0 << 12 | 0b10 | 1 << 9;
    uint64_t func                       = get_current_task()->main;

    // Message threads enter user mode through the same syscall path.
    write_kgsbase((uint64_t)get_current_cpu());
    write_gsbase(0);
	
    __asm__ volatile("mov %0, %%es\n"
                     "mov %0, %%ds\n"
                     "pushq %5\n"      // SS
                     "pushq %1\n"      // RSP
                     "pushq %2\n"      // RFLAGS
                     "pushq %3\n"      // CS
                     "pushq %4\n"      // RIP
                     "mov %6, %%rdi\n" // arg1
                     "iretq\n"
                     :
                     : "r"((uint64_t)GET_SEL(4 * 8, SA_RPL3)), "r"((uint64_t)rsp),
                       "r"(get_current_task()->context0.rflags), "r"((uint64_t)SELECTOR_USER_CS), "r"(func),
                       "r"((uint64_t)SELECTOR_USER_DS),
                       "r"(tcb)
                     : "memory");
}

size_t envp_length(char **envp)
{
    size_t count = 0;
    while (envp[count] != NULL)
    {
        count++;
    }
    return count;
}

char **copy_envp(char **envp)
{
    size_t count = 0;
    while (envp[count] != NULL)
    {
        count++;
    }
    char **new_envp = (char **)malloc((count + 1) * sizeof(char *));
    if (!new_envp) return NULL;
    for (size_t i = 0; i < count; i++)
    {
        new_envp[i] = strdup(envp[i]);
        if (!new_envp[i])
        {
            for (size_t j = 0; j < i; j++)
            {
                free(new_envp[j]);
            }
            free(new_envp);
            return NULL;
        }
    }
    new_envp[count] = NULL;
    return new_envp;
}

static int copy_exec_string_from_user(page_directory_t *pagedir, char **out, const char *src, size_t *total_bytes)
{
    if (out == NULL) return -EINVAL;
    *out = NULL;
    if (pagedir == NULL || src == NULL) return -EFAULT;

    char *tmp = (char *)malloc(EXECVE_STRING_MAX);
    if (tmp == NULL) return -ENOMEM;

    for (size_t i = 0; i < EXECVE_STRING_MAX; i++)
    {
        char c = 0;
        if (!copy_from_user_pagedir(pagedir, &c, src + i, sizeof(c)))
        {
            free(tmp);
            return -EFAULT;
        }
        tmp[i] = c;
        if (c == '\0')
        {
            size_t used = i + 1;
            if (total_bytes != NULL)
            {
                if (*total_bytes > EXECVE_TOTAL_STRING_MAX - used)
                {
                    free(tmp);
                    return -E2BIG;
                }
                *total_bytes += used;
            }
            *out = tmp;
            return 0;
        }
    }

    free(tmp);
    return -ENAMETOOLONG;
}

static int copy_exec_string_vector_from_user(page_directory_t *pagedir, char **user_vec, char ***out_vec,
                                             size_t *out_count)
{
    if (out_vec == NULL || out_count == NULL) return -EINVAL;
    *out_vec = NULL;
    *out_count = 0;
    if (user_vec == NULL) return 0;

    char **vec = (char **)calloc(EXECVE_VECTOR_MAX + 1, sizeof(char *));
    if (vec == NULL) return -ENOMEM;

    size_t total_bytes = 0;
    for (size_t i = 0; i < EXECVE_VECTOR_MAX; i++)
    {
        char *user_str = NULL;
        if (!copy_from_user_pagedir(pagedir, &user_str, &user_vec[i], sizeof(user_str)))
        {
            free_envp(vec);
            return -EFAULT;
        }
        if (user_str == NULL)
        {
            *out_vec = vec;
            *out_count = i;
            return 0;
        }

        int ret = copy_exec_string_from_user(pagedir, &vec[i], user_str, &total_bytes);
        if (ret < 0)
        {
            free_envp(vec);
            return ret;
        }
    }

    char *extra = NULL;
    if (copy_from_user_pagedir(pagedir, &extra, &user_vec[EXECVE_VECTOR_MAX], sizeof(extra)) && extra == NULL)
    {
        *out_vec = vec;
        *out_count = EXECVE_VECTOR_MAX;
        return 0;
    }

    free_envp(vec);
    return -E2BIG;
}

static char **copy_kernel_string_vector(char **argv, size_t argc)
{
    if (argv == NULL || argc == 0) return NULL;

    char **copy = (char **)calloc(argc + 1, sizeof(char *));
    if (copy == NULL) return NULL;

    for (size_t i = 0; i < argc; i++)
    {
        if (argv[i] == NULL)
        {
            free_envp(copy);
            return NULL;
        }
        copy[i] = strdup(argv[i]);
        if (copy[i] == NULL)
        {
            free_envp(copy);
            return NULL;
        }
    }
    copy[argc] = NULL;
    return copy;
}

static char *dup_trimmed_slice(const char *start, const char *end)
{
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    if (end <= start) return NULL;

    size_t len = (size_t)(end - start);
    char  *out = (char *)malloc(len + 1);
    if (out == NULL) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int rewrite_shebang_exec(vfs_node_t *node, char **norm_path, char ***argv, size_t *argc)
{
    if (node == NULL || *node == NULL || norm_path == NULL || *norm_path == NULL || argv == NULL || argc == NULL)
        return 0;

    char header[EXECVE_STRING_MAX];
    memset(header, 0, sizeof(header));
    size_t got = vfs_read(*node, header, 0, sizeof(header) - 1);
    if (got < 2 || header[0] != '#' || header[1] != '!') return 0;

    char *line_end = header + 2;
    char *limit = header + got;
    while (line_end < limit && *line_end != '\n') line_end++;

    char *cursor = header + 2;
    while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) cursor++;
    char *interp_start = cursor;
    while (cursor < line_end && *cursor != ' ' && *cursor != '\t' && *cursor != '\r') cursor++;
    char *interp = dup_trimmed_slice(interp_start, cursor);
    if (interp == NULL) return -ENOEXEC;

    char *interp_arg = dup_trimmed_slice(cursor, line_end);
    size_t old_argc = *argc;
    size_t extra = interp_arg != NULL ? 3 : 2;
    if (old_argc > EXECVE_VECTOR_MAX - extra)
    {
        free(interp);
        free(interp_arg);
        return -E2BIG;
    }

    char **new_argv = (char **)calloc(old_argc + extra + 1, sizeof(char *));
    if (new_argv == NULL)
    {
        free(interp);
        free(interp_arg);
        return -ENOMEM;
    }

    size_t out_i = 0;
    new_argv[out_i++] = strdup(interp);
    if (new_argv[out_i - 1] == NULL)
    {
        free_envp(new_argv);
        free(interp);
        free(interp_arg);
        return -ENOMEM;
    }
    if (interp_arg != NULL)
    {
        new_argv[out_i++] = interp_arg;
        interp_arg = NULL;
    }
    new_argv[out_i++] = strdup(*norm_path);
    if (new_argv[out_i - 1] == NULL)
    {
        free_envp(new_argv);
        free(interp);
        free(interp_arg);
        return -ENOMEM;
    }
    for (size_t i = 1; i < old_argc; i++)
    {
        new_argv[out_i++] = strdup((*argv)[i]);
        if (new_argv[out_i - 1] == NULL)
        {
            free_envp(new_argv);
            free(interp);
            free(interp_arg);
            return -ENOMEM;
        }
    }
    new_argv[out_i] = NULL;

    vfs_node_t interp_node = vfs_open(interp);
    if (interp_node == NULL)
    {
        free_envp(new_argv);
        free(interp);
        return -ENOENT;
    }

    vfs_close(*node);
    *node = interp_node;
    free(*norm_path);
    *norm_path = interp;
    free_envp(*argv);
    *argv = new_argv;
    *argc = out_i;
    return 0;
}

//from cops但契合xj380

static constexpr uint64_t CLONE_VM_FLAG             = 0x00000100;//呃呃呃呃呃呃呃呃·
static constexpr uint64_t CLONE_FS_FLAG             = 0x00000200;
static constexpr uint64_t CLONE_FILES_FLAG          = 0x00000400;
static constexpr uint64_t CLONE_SIGHAND_FLAG        = 0x00000800;
static constexpr uint64_t CLONE_VFORK_FLAG          = 0x00004000;
static constexpr uint64_t CLONE_THREAD_FLAG         = 0x00010000;
static constexpr uint64_t CLONE_SETTLS_FLAG         = 0x00080000;
static constexpr uint64_t CLONE_PARENT_SETTID_FLAG  = 0x00100000;
static constexpr uint64_t CLONE_CHILD_CLEARTID_FLAG = 0x00200000;
static constexpr uint64_t CLONE_CHILD_SETTID_FLAG   = 0x01000000;

uint64_t thread_clone(struct X64_REGS *reg, uint64_t flags, uint64_t stack, int *parent_tid, int *child_tid,
                      uint64_t tls)
{
    if (reg == NULL) return SYSCALL_FAULT_(EINVAL);

    if ((flags & CLONE_THREAD_FLAG) == 0)
        return process_fork(reg, (flags & CLONE_VFORK_FLAG) != 0, stack, flags, parent_tid, child_tid);

    const uint64_t required = CLONE_VM_FLAG | CLONE_FS_FLAG | CLONE_FILES_FLAG | CLONE_SIGHAND_FLAG;
    if ((flags & required) != required) return SYSCALL_FAULT_(EINVAL);

    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    tcb_t parent_task          = get_current_task();
    if (parent_task == NULL || parent_task->parent_group == NULL || parent_task->parent_group->pagedir == NULL)
    {
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(EFAULT);
    }
    uint64_t parent_fs_base = read_fsbase();
    if (parent_fs_base != 0 || parent_task->fs_base == 0)
        parent_task->fs_base = parent_fs_base;

    if ((flags & CLONE_PARENT_SETTID_FLAG) && parent_tid == NULL) return SYSCALL_FAULT_(EFAULT);
    if ((flags & (CLONE_CHILD_SETTID_FLAG | CLONE_CHILD_CLEARTID_FLAG)) && child_tid == NULL)
        return SYSCALL_FAULT_(EFAULT);

    close_interrupt;
    disable_scheduler();

    spin_lock(&create_thread_lock);

    pcb_t process = parent_task->parent_group;
    if (process->exec_in_progress)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(EAGAIN);
    }
    page_directory_t *pagedir = process->pagedir;

    tcb_t new_task = alloc_zeroed_tcb();
    if (new_task == NULL)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(ENOMEM);
    }

    new_task->task_level = TASK_APPLICATION_LEVEL;
    new_task->cpu_id     = get_current_cpu()->processor_id;
    new_task->status     = START;
    new_task->parent_group = process;
    strncpy(new_task->name, parent_task->name, sizeof(new_task->name) - 1);

    uint64_t kernel_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (kernel_stack_phys == 0)
    {
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(ENOMEM);
    }
    uint64_t syscall_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (syscall_stack_phys == 0)
    {
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(ENOMEM);
    }

    new_task->kernel_stack  = (uint64_t)phys_to_virt(kernel_stack_phys) + KERNEL_STACK_SIZE;
    new_task->syscall_stack = (uint64_t)phys_to_virt(syscall_stack_phys) + KERNEL_STACK_SIZE;
    new_task->main          = parent_task->main;
    new_task->user_stack    = stack != 0 ? stack : parent_task->user_stack;
    new_task->user_stack_top = stack != 0 ? stack : parent_task->user_stack_top;

    new_task->context0.rip    = reg->rcx;
    new_task->context0.rflags = reg->r11;
    new_task->context0.cs     = reg->cs;
    new_task->context0.ss     = reg->ss;
    new_task->context0.es     = reg->es;
    new_task->context0.ds     = reg->ds;
    new_task->context0.rax    = 0;//子进程返回零
    new_task->context0.rdi    = reg->rdi;
    new_task->context0.rsi    = reg->rsi;
    new_task->context0.rdx    = reg->rdx;
    new_task->context0.r9     = reg->r9;
    new_task->context0.r8     = reg->r8;
    new_task->context0.r10    = reg->r10;
    new_task->context0.r11    = reg->r11;
    new_task->context0.r12    = reg->r12;
    new_task->context0.r13    = reg->r13;
    new_task->context0.r14    = reg->r14;
    new_task->context0.r15    = reg->r15;
    new_task->context0.rbx    = reg->rbx;
    new_task->context0.rbp    = reg->rbp;
    new_task->context0.rsp    = stack != 0 ? stack : reg->rsp;
    new_task->context0.rcx    = reg->rcx;

    memcpy(new_task->fpu_context.fxsave_area, parent_task->fpu_context.fxsave_area, 512);
    memcpy(new_task->actions, parent_task->actions, sizeof(new_task->actions));
    new_task->blocked = parent_task->blocked;
    new_task->signal  = 0;
    new_task->fs      = parent_task->fs;
    new_task->fs_base = (flags & CLONE_SETTLS_FLAG) ? tls : parent_task->fs_base;

    new_task->cwd = parent_task->cwd;
    if (new_task->cwd != NULL) new_task->cwd->refcount++;
    new_task->str_cwd = parent_task->str_cwd != NULL ? strdup(parent_task->str_cwd) : NULL;
    new_task->user_info = parent_task->user_info;
    new_task->winnum = 0;
    new_task->hasfscr = false;
    new_task->window_count = 0;
    new_task->tid = alloc_tid();
    int tid = (int)new_task->tid;
    if ((flags & CLONE_PARENT_SETTID_FLAG) && !copy_to_user_pagedir(pagedir, parent_tid, &tid, sizeof(tid)))
    {
        if (new_task->cwd != NULL) vfs_close(new_task->cwd);
        free(new_task->str_cwd);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(EFAULT);
    }
    if ((flags & CLONE_CHILD_SETTID_FLAG) && !copy_to_user_pagedir(pagedir, child_tid, &tid, sizeof(tid)))
    {
        if (new_task->cwd != NULL) vfs_close(new_task->cwd);
        free(new_task->str_cwd);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(EFAULT);
    }
    if (flags & CLONE_CHILD_CLEARTID_FLAG)
    {
        new_task->clear_child_tid = (uint64_t)child_tid;
        new_task->tid_directory   = pagedir;
    }

    new_task->group_index = queue_enqueue(process->thread_queue, new_task);
    if (new_task->group_index == (size_t)-1)
    {
        if (new_task->cwd != NULL) vfs_close(new_task->cwd);
        free(new_task->str_cwd);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return SYSCALL_FAULT_(ENOMEM);
    }

    add_task(new_task);
    spin_unlock(&create_thread_lock);
    restore_runtime_state(was_scheduler_enabled, is_sti);
    return new_task->tid;
}



uint64_t process_execve(char *path, char **argv, char **envp)
{
    // while (true)
    // {
    //     if (is_scheduler) break;
    // }
    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    if (!no_interrupt) open_interrupt;
    if (!was_scheduler_enabled) enable_scheduler();

    if (path == NULL)
    {
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EINVAL;
    }

    tcb_t current_task = get_current_task();
    if (current_task == NULL || current_task->parent_group == NULL || current_task->parent_group->pagedir == NULL)
    {
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EFAULT;
    }

    page_directory_t *caller_pagedir = current_task->parent_group->pagedir;
    char  *kpath = NULL;
    char **kargv = NULL;
    char **kenvp = NULL;
    size_t kargc = 0;
    size_t kenvc = 0;
    char **new_task_argv = NULL;
    char **new_process_argv = NULL;
    char *new_exe_path = NULL;

    int copy_ret = copy_exec_string_from_user(caller_pagedir, &kpath, path, NULL);
    if (copy_ret < 0)
    {
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)copy_ret;
    }
    copy_ret = copy_exec_string_vector_from_user(caller_pagedir, argv, &kargv, &kargc);
    if (copy_ret < 0)
    {
        free(kpath);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)copy_ret;
    }
    copy_ret = copy_exec_string_vector_from_user(caller_pagedir, envp, &kenvp, &kenvc);
    if (copy_ret < 0)
    {
        free(kpath);
        free_envp(kargv);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)copy_ret;
    }

    pcb_t process = current_task->parent_group;
    bool was_vfork = process->vfork;
    ipc_message_t vfork_exec_message = NULL;

    char      *norm_path = vfs_cwd_path_build(kpath);
    if (norm_path == NULL)
    {
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }
    vfs_node_t node      = vfs_open(norm_path);
    if (node == NULL)
    {
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOENT;
    }
    int shebang_ret = rewrite_shebang_exec(&node, &norm_path, &kargv, &kargc);
    if (shebang_ret < 0)
    {
        vfs_close(node);
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)shebang_ret;
    }

    if (process->thread_queue == NULL)
    {
        vfs_close(node);
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EAGAIN;
    }

    spin_lock(&create_thread_lock);
    spin_lock(&process->thread_queue->lock);
    bool can_transition = !process->exec_in_progress && process->thread_queue->size == 1;
    if (can_transition) process->exec_in_progress = true;
    spin_unlock(&process->thread_queue->lock);
    spin_unlock(&create_thread_lock);
    if (!can_transition)
    {
        vfs_close(node);
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EAGAIN;
    }

    close_interrupt;
    spin_lock(&execve_image_lock);
    char new_process_name[sizeof(process->name)];
    get_thread_name_from_filepath(norm_path, new_process_name);
    UserInfo *exec_user = current_user != NULL ? current_user : &root_user;
    char **candidate_envp = envp != NULL ? kenvp : exec_user->envp;
    size_t candidate_envc = envp != NULL ? kenvc : exec_user->envc;
    user_image_candidate_context_t candidate;
    int candidate_result = user_image_prepare_candidate(&candidate, norm_path, new_process_name, kargv,
                                                        candidate_envp, candidate_envc, exec_user);
    if (candidate_result < 0)
    {
        spin_unlock(&execve_image_lock);
        finish_execve_transition(process);
        vfs_close(node);
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)candidate_result;
    }

    new_task_argv = copy_kernel_string_vector_owned(candidate.argv, candidate.argc);
    if (candidate.argc != 0 && new_task_argv == NULL)
    {
        user_image_abort(&candidate);
        spin_unlock(&execve_image_lock);
        finish_execve_transition(process);
        vfs_close(node);
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }
    if (was_vfork)
    {
        vfork_exec_message = (ipc_message_t)calloc(1, sizeof(struct ipc_message));
        if (vfork_exec_message == NULL)
        {
            free_envp(new_task_argv);
            user_image_abort(&candidate);
            spin_unlock(&execve_image_lock);
            finish_execve_transition(process);
            vfs_close(node);
            free(norm_path);
            free(kpath);
            free_envp(kargv);
            free_envp(kenvp);
            restore_runtime_state(was_scheduler_enabled, is_sti);
            return (uint64_t)-ENOMEM;
        }
    }
    user_image_candidate_discard_elf_buffers(&candidate);

    if (!freeze_process_for_exec(process))
    {
        free_envp(new_task_argv);
        free(vfork_exec_message);
        user_image_abort(&candidate);
        unfreeze_process(process);
        spin_unlock(&execve_image_lock);
        finish_execve_transition(process);
        vfs_close(node);
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EAGAIN;
    }

    user_image_process_state_t new_process_image = {};
    user_image_snapshot_t old_image;
    user_image_snapshot_init(&old_image);
    if (!user_image_commit_locked(&new_process_image, &candidate, &old_image))
    {
        free_envp(new_task_argv);
        free(vfork_exec_message);
        user_image_abort(&candidate);
        spin_unlock(&execve_image_lock);
        finish_execve_transition(process);
        vfs_close(node);
        free(norm_path);
        free(kpath);
        free_envp(kargv);
        free_envp(kenvp);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EINVAL;
    }

    process->pagedir = new_process_image.pagedir;
    process->vma_manager = new_process_image.vma_manager;
    process->virt_queue = new_process_image.virt_queue;
    process->exe_path = new_process_image.exe_path;
    process->cmdline = new_process_image.cmdline;
    process->argv = new_process_image.argv;
    process->argc = new_process_image.argc;
    process->envp = new_process_image.envp;
    process->envc = new_process_image.envc;
    process->aux_phdr = new_process_image.aux_phdr;
    process->aux_phent = new_process_image.aux_phent;
    process->aux_phnum = new_process_image.aux_phnum;
    process->aux_base = new_process_image.aux_base;
    process->aux_entry = new_process_image.aux_entry;
    process->aux_execfn = new_process_image.aux_execfn;
    process->elf_file = NULL;
    process->elf_size = 0;
    process->load_start = 0;
    process->prepared_user_stack = 0;
    process->prepared_user_stack_top = 0;
    process->prepared_user_rsp = 0;
    process->prepared_user_argv = 0;
    process->prepared_user_envp = 0;
    process->prepared_user_entry_rdx = 0;
    process->brk_start = USER_BRK_START;
    process->brk_end = USER_BRK_END;
    process->brk_current = USER_BRK_START;
    process->mmap_start = USER_MMAP_START;
    process->vfork = false;
    strncpy(process->name, new_process_name, sizeof(process->name) - 1);
    strncpy(current_task->name, new_process_name, sizeof(current_task->name) - 1);

    char **old_task_argv = current_task->argv;
    current_task->argv = new_task_argv;
    current_task->argc = new_process_image.argc;
    current_task->user_stack = new_process_image.user_stack;
    current_task->user_stack_top = new_process_image.user_stack_top;
    current_task->owns_user_stack = true;
    current_task->uses_prepared_user_stack = true;
    current_task->prepared_user_rsp = new_process_image.initial_rsp;
    current_task->prepared_user_argv = new_process_image.initial_argv;
    current_task->prepared_user_envp = new_process_image.initial_envp;
    current_task->prepared_user_entry_rdx = new_process_image.entry_rdx;
    current_task->main = new_process_image.entry;
    current_task->hasfscr = false;
    current_task->winnum = 0;
    current_task->fs = GET_SEL(4 * 8, SA_RPL3);
    current_task->fs_base = 0;
    if (old_task_argv != NULL) free_envp(old_task_argv);
    close_exec_file_descriptors(process);
    if (current_task->tid_directory == old_image.image.pagedir) current_task->tid_directory = process->pagedir;

    if (was_vfork) old_image.image.pagedir = NULL;
    if (!user_image_retirement_enqueue(&execve_retirement_queue, &old_image))
        write_serial_string("execve: old image retirement queue allocation failed; retaining old image.\n");
    vfs_close(node);
    free(norm_path);
    free(kpath);
    free_envp(kargv);
    free_envp(kenvp);
    switch_process_page_directory(process->pagedir);
    /* Single-thread admission means no other runnable TCB can still use old_image. */
    user_image_retirement_mark_quiescent(&execve_retirement_queue);
    user_image_retirement_drain(&execve_retirement_queue);
    if (was_vfork)
    {
        vfork_exec_message->type = IPC_MSG_TYPE_EXEC;
        vfork_exec_message->pid = process->pid;
        if (process->parent_task != NULL) ipc_send(process->parent_task, vfork_exec_message);
        else free(vfork_exec_message);
    }
    __asm__ __volatile__("movq %0, %%fs\n\t" : : "r"(task_user_fs_selector(current_task)));
    write_fsbase(0);
    finish_execve_transition(process);
    unfreeze_process(process);
    spin_unlock(&execve_image_lock);
    if (was_scheduler_enabled) enable_scheduler();
    switch_task_to_user_mode(current_task);
    return (uint64_t)-(EAGAIN);

}

static void *file_copy(void *ptr)
{
    if (ptr == NULL) return NULL;
    fd_file_handle *src = (fd_file_handle *)ptr;
    return fd_dup(src);
}

static void close_exec_file_descriptors(pcb_t process)
{
    if (process == NULL || process->file_open == NULL) return;

    while (true)
    {
        size_t fd_to_close = (size_t)-1;

        spin_lock(&process->file_open->lock);
        lock_node *node = process->file_open->head;
        while (node != NULL)
        {
            fd_file_handle *handle = (fd_file_handle *)node->data;
            if (handle != NULL && (handle->flags & O_CLOEXEC))
            {
                fd_to_close = node->index;
                break;
            }
            node = node->next;
        }
        spin_unlock(&process->file_open->lock);

        if (fd_to_close == (size_t)-1) break;

        fd_file_handle *handle = (fd_file_handle *)queue_remove_at(process->file_open, fd_to_close);
        if (handle != NULL)
        {
            vfs_close(handle->node);
            free(handle);
        }
    }
}

static void free_fd_handle_queue(lock_queue *queue)
{
    if (queue == NULL) return;
    while (true)
    {
        fd_file_handle *handle = (fd_file_handle *)queue_dequeue(queue);
        if (handle == NULL) break;
        vfs_close(handle->node);
        free(handle);
    }
    queue_destroy(queue);
}

static size_t *alloc_fd_table_ref(void)
{
    size_t *refs = (size_t *)malloc(sizeof(size_t));
    if (refs != NULL) *refs = 1;
    return refs;
}

static void fd_table_ref(pcb_t pcb)
{
    if (pcb == NULL || pcb->file_open_shared_refs == NULL) return;
    __atomic_fetch_add(pcb->file_open_shared_refs, 1, __ATOMIC_SEQ_CST);
}

static bool fd_table_unref(pcb_t pcb)
{
    if (pcb == NULL || pcb->file_open_shared_refs == NULL) return true;
    size_t old = __atomic_fetch_sub(pcb->file_open_shared_refs, 1, __ATOMIC_SEQ_CST);
    if (old <= 1)
    {
        free(pcb->file_open_shared_refs);
        pcb->file_open_shared_refs = NULL;
        return true;
    }
    pcb->file_open_shared_refs = NULL;
    return false;
}

static void close_process_file_table(pcb_t pcb)
{
    if (pcb == NULL || pcb->file_open == NULL) return;

    lock_queue *file_open = pcb->file_open;
    bool close_fd_table = fd_table_unref(pcb);
    pcb->file_open = NULL;

    if (!close_fd_table) return;

    while (true)
    {
        fd_file_handle *handle = (fd_file_handle *)queue_dequeue(file_open);
        if (handle == NULL) break;
        vfs_close(handle->node);
        free(handle);
    }
    queue_destroy(file_open);
}

static void free_owned_pointer_queue(lock_queue *queue)
{
    if (queue == NULL) return;
    while (true)
    {
        void *ptr = queue_dequeue(queue);
        if (ptr == NULL) break;
        free(ptr);
    }
    queue_destroy(queue);
}

static void free_partial_process(pcb_t pcb, bool owns_pagedir)
{
    if (pcb == NULL) return;

    if (pcb->parent_task != NULL && pcb->parent_task->child_pcb != NULL && pcb->child_index != (size_t)-1)
        queue_remove_data(pcb->parent_task->child_pcb, pcb);
    if (pcb_group_queue != NULL && pcb->queue_index != (size_t)-1)
        queue_remove_data(pcb_group_queue, pcb);

    if (pcb->thread_queue) queue_destroy(pcb->thread_queue);
    if (pcb->child_pcb) queue_destroy(pcb->child_pcb);
    if (fd_table_unref(pcb)) free_fd_handle_queue(pcb->file_open);
    free_owned_pointer_queue(pcb->virt_queue);
    free_owned_pointer_queue(pcb->ipc_queue);
    vma_manager_exit_cleanup(&pcb->vma_manager);
    if (owns_pagedir) free_page_directory(pcb->pagedir);
    free_tty(pcb->tty);
    free(pcb->cmdline);
    free_envp(pcb->argv);
    free(pcb->exe_path);
    free(pcb->xtttp_stc);
    free(pcb);
}

static void free_fork_task(tcb_t task)
{
    if (task == NULL) return;
    if (task->cwd != NULL) vfs_close(task->cwd);
    free(task->str_cwd);
    if (task->kernel_stack != 0)
        free_frames((uint64_t)virt_to_phys((task->kernel_stack - KERNEL_STACK_SIZE)), KERNEL_STACK_SIZE / PAGE_SIZE);
    if (task->syscall_stack != 0)
        free_frames((uint64_t)virt_to_phys((task->syscall_stack - KERNEL_STACK_SIZE)), KERNEL_STACK_SIZE / PAGE_SIZE);
    free(task);
}

static void init_fork_child_context(tcb_t child, const struct X64_REGS *frame, uint64_t user_stack)
{
    child->context0.rip    = frame->rcx;
    child->context0.rflags = frame->r11;
    child->context0.cs     = frame->cs;
    child->context0.ss     = frame->ss;
    child->context0.es     = frame->es;
    child->context0.ds     = frame->ds;
    child->context0.rax    = 0;
    child->context0.rdi    = frame->rdi;
    child->context0.rsi    = frame->rsi;
    child->context0.rdx    = frame->rdx;
    child->context0.r9     = frame->r9;
    child->context0.r8     = frame->r8;
    child->context0.r10    = frame->r10;
    child->context0.r11    = frame->r11;
    child->context0.r12    = frame->r12;
    child->context0.r13    = frame->r13;
    child->context0.r14    = frame->r14;
    child->context0.r15    = frame->r15;
    child->context0.rbx    = frame->rbx;
    child->context0.rbp    = frame->rbp;
    child->context0.rsp    = user_stack == 0 ? frame->rsp : user_stack;
    child->context0.rcx    = frame->rcx;
}

static void set_child_user_stack_bounds(tcb_t child, tcb_t parent, uint64_t user_stack)
{
    if (child == NULL || parent == NULL) return;

    if (user_stack == 0)
    {
        child->user_stack     = parent->user_stack;
        child->user_stack_top = parent->user_stack_top;
        return;
    }

    uint64_t stack_top = (user_stack + PAGE_SIZE - 1) & PAGE_MASK;
    if (stack_top < user_stack || stack_top < BIG_USER_STACK)
    {
        child->user_stack     = parent->user_stack;
        child->user_stack_top = parent->user_stack_top;
        return;
    }

    child->user_stack     = stack_top - BIG_USER_STACK;
    child->user_stack_top = stack_top;
}

uint64_t process_fork(struct X64_REGS *reg, bool is_vfork, uint64_t user_stack, uint64_t clone_flags,
                      int *parent_tid, int *child_tid)
{
    if (reg == NULL) return (uint64_t)-EINVAL;

    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    close_interrupt;
    disable_scheduler();

    tcb_t parent_task = get_current_task();
    if (parent_task == NULL || parent_task->parent_group == NULL)
    {
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EFAULT;
    }

    struct X64_REGS fork_frame = *reg;
    uint64_t parent_fs_base = read_fsbase();
    if (parent_fs_base != 0 || parent_task->fs_base == 0)
        parent_task->fs_base = parent_fs_base;

    pcb_t current_pcb = parent_task->parent_group;
    spin_lock(&create_thread_lock);
    if (current_pcb->exec_in_progress)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EAGAIN;
    }
    if ((clone_flags & CLONE_PARENT_SETTID_FLAG) && parent_tid == NULL)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EFAULT;
    }
    if ((clone_flags & (CLONE_CHILD_SETTID_FLAG | CLONE_CHILD_CLEARTID_FLAG)) && child_tid == NULL)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EFAULT;
    }

    pcb_t new_pcb = (pcb_t)malloc(sizeof(struct process_control_block));
    if (new_pcb == NULL)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }
    memset(new_pcb, 0, sizeof(struct process_control_block));
    new_pcb->queue_index = (size_t)-1;
    new_pcb->child_index = (size_t)-1;
    new_pcb->pid = alloc_pid();
    strcpy(new_pcb->name, current_pcb->name);
    write_serial_fmt("fork name: %s\n", new_pcb->name);
    new_pcb->task_level = TASK_APPLICATION_LEVEL;
    new_pcb->status     = START;
    new_pcb->vfork      = is_vfork;
    new_pcb->ppid       = current_pcb->pid;
    new_pcb->load_start = current_pcb->load_start;
    new_pcb->aux_phdr   = current_pcb->aux_phdr;
    new_pcb->aux_phent  = current_pcb->aux_phent;
    new_pcb->aux_phnum  = current_pcb->aux_phnum;
    new_pcb->aux_base   = current_pcb->aux_base;
    new_pcb->aux_entry  = current_pcb->aux_entry;
    new_pcb->aux_execfn = current_pcb->aux_execfn;
    new_pcb->elf_size   = current_pcb->elf_size;

    new_pcb->pagedir = is_vfork ? current_pcb->pagedir : clone_page_directory(current_pcb->pagedir, false);
    if (new_pcb->pagedir == NULL)
    {
        free(new_pcb);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }

    if (!vma_manager_clone(&current_pcb->vma_manager, &new_pcb->vma_manager))//calloc灏辨垜鐙楀懡
    {
        write_serial_fmt("task: cannot clone process vma information.\n");
        free_partial_process(new_pcb, !is_vfork);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return -ENOMEM;
    }
    new_pcb->brk_start   = current_pcb->brk_start;
    new_pcb->brk_end     = current_pcb->brk_end;
    new_pcb->brk_current = current_pcb->brk_current;
    new_pcb->mmap_start  = current_pcb->mmap_start;
    new_pcb->cmdline      = strdup(current_pcb->cmdline);
    new_pcb->argv         = copy_kernel_string_vector_owned(current_pcb->argv, current_pcb->argc);
    new_pcb->argc         = current_pcb->argc;
    new_pcb->exe_path     = current_pcb->exe_path ? strdup(current_pcb->exe_path) : NULL;
    new_pcb->thread_queue = queue_init();
    new_pcb->parent_task  = current_pcb;
    new_pcb->child_pcb    = queue_init();
    new_pcb->virt_queue   = queue_copy(current_pcb->virt_queue, virt_copy);
    if (clone_flags & CLONE_FILES_FLAG)
    {
        new_pcb->file_open = current_pcb->file_open;
        new_pcb->file_open_shared_refs = current_pcb->file_open_shared_refs;
        fd_table_ref(new_pcb);
    }
    else
    {
        new_pcb->file_open = queue_copy(current_pcb->file_open, file_copy);
        new_pcb->file_open_shared_refs = alloc_fd_table_ref();
    }
    new_pcb->ipc_queue    = queue_init();
    new_pcb->tty          = alloc_default_tty();

    new_pcb->xtttp_stc = (xtttp_dtt *)malloc(sizeof(xtttp_dtt));
    if (new_pcb->cmdline == NULL || (current_pcb->argc > 0 && new_pcb->argv == NULL) ||
        (current_pcb->exe_path != NULL && new_pcb->exe_path == NULL) ||
        new_pcb->thread_queue == NULL || new_pcb->child_pcb == NULL ||
        new_pcb->virt_queue == NULL || new_pcb->file_open == NULL || new_pcb->file_open_shared_refs == NULL ||
        new_pcb->ipc_queue == NULL ||
        new_pcb->tty == NULL || new_pcb->xtttp_stc == NULL)
    {
        free_partial_process(new_pcb, !is_vfork);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }
    memset(new_pcb->xtttp_stc, 0, sizeof(xtttp_dtt));

    new_pcb->queue_index = queue_enqueue(pcb_group_queue, new_pcb);
    if (new_pcb->queue_index == (size_t)-1)
    {
        free_partial_process(new_pcb, !is_vfork);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }

    new_pcb->child_index = queue_enqueue(current_pcb->child_pcb, new_pcb);
    if (new_pcb->child_index == (size_t)-1)
    {
        free_partial_process(new_pcb, !is_vfork);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }

    tcb_t new_task = alloc_zeroed_tcb();
    if (new_task == NULL)
    {
        free_partial_process(new_pcb, !is_vfork);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-(ENOMEM);
    }
    new_task->task_level     = TASK_APPLICATION_LEVEL;
    new_task->cpu_id         = get_current_cpu()->processor_id;
    new_task->status         = START;
    set_child_user_stack_bounds(new_task, parent_task, user_stack);
    uint64_t kernel_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (kernel_stack_phys == 0)
    {
        free_partial_process(new_pcb, !is_vfork);
        free_fork_task(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }
    uint64_t syscall_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (syscall_stack_phys == 0)
    {
        free_partial_process(new_pcb, !is_vfork);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_fork_task(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }
    new_task->kernel_stack  = ((uint64_t)phys_to_virt(kernel_stack_phys)) + KERNEL_STACK_SIZE;
    new_task->syscall_stack = ((uint64_t)phys_to_virt(syscall_stack_phys)) + KERNEL_STACK_SIZE;
    new_task->main          = parent_task->main;
    new_task->cwd           = parent_task->cwd;
    if (new_task->cwd != NULL) new_task->cwd->refcount++;
    new_task->str_cwd       = parent_task->str_cwd != NULL ? strdup(parent_task->str_cwd) : NULL;
    new_task->user_info     = parent_task->user_info;
    strcpy(new_task->name, parent_task->name);
    new_task->winnum  = 0;
    new_task->hasfscr = false;

    init_fork_child_context(new_task, &fork_frame, user_stack);

    memcpy(new_task->fpu_context.fxsave_area, parent_task->fpu_context.fxsave_area, 512);
    memcpy(new_task->actions, parent_task->actions, sizeof(new_task->actions));
    new_task->blocked = parent_task->blocked;
    new_task->signal  = 0;

    new_task->fs      = parent_task->fs;
    new_task->fs_base = parent_task->fs_base;

    new_task->parent_group = new_pcb;
    new_task->tid = new_pcb->pid;
    reserve_tid_value(new_task->tid);
    int child_tid_value = (int)new_task->tid;
    if ((clone_flags & CLONE_PARENT_SETTID_FLAG) &&
        !copy_to_user_pagedir(current_pcb->pagedir, parent_tid, &child_tid_value, sizeof(child_tid_value)))
    {
        free_partial_process(new_pcb, !is_vfork);
        free_fork_task(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EFAULT;
    }
    if ((clone_flags & CLONE_CHILD_SETTID_FLAG) &&
        !copy_to_user_pagedir(new_pcb->pagedir, child_tid, &child_tid_value, sizeof(child_tid_value)))
    {
        free_partial_process(new_pcb, !is_vfork);
        free_fork_task(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-EFAULT;
    }
    if (clone_flags & CLONE_CHILD_CLEARTID_FLAG)
    {
        new_task->clear_child_tid = (uint64_t)child_tid;
        new_task->tid_directory   = new_pcb->pagedir;
    }
    new_task->group_index  = queue_enqueue(new_pcb->thread_queue, new_task);
    if (new_task->group_index == (size_t)-1)
    {
        free_partial_process(new_pcb, !is_vfork);
        free_fork_task(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }

    new_task->window_count = 0;

    procfs_on_new_task(new_pcb);
    if (add_task(new_task) == (size_t)-1)
    {
        queue_remove_at(new_pcb->thread_queue, new_task->group_index);
        free_partial_process(new_pcb, !is_vfork);
        free_fork_task(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (uint64_t)-ENOMEM;
    }
    spin_unlock(&create_thread_lock);
    restore_runtime_state(was_scheduler_enabled, is_sti);

    write_serial_fmt("FORK PID: %d\n", new_pcb->pid);
    if (!is_vfork) return new_pcb->pid;

    scheduler_yield();
    while (true)
    {
        ipc_message_t msg = ipc_recv_wait2(IPC_MSG_TYPE_EXEC, IPC_MSG_TYPE_EPID);
        if (msg->pid == new_pcb->pid)
        {
            free(msg);
            return new_pcb->pid;
        }
        ipc_send(current_pcb->parent_task, msg);
    }

    int npid = new_pcb->pid;
    do
    {
        ipc_message_t msg = ipc_recv_wait(IPC_MSG_TYPE_EXEC);
        if (npid == msg->pid)
        {
            free(msg);
            return npid;
        }
        ipc_send(current_pcb->parent_task, msg);
    } while (true);
}

size_t create_kernel_thread(void *_start, void *args, char *name, pcb_t pcb)
{
    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    if (!no_interrupt) open_interrupt;

    spin_lock(&create_thread_lock);
    close_interrupt;
    disable_scheduler();
    pcb_t target_group = pcb == NULL ? kernel_group : pcb;
    if (_start == NULL || name == NULL || target_group == NULL || target_group->thread_queue == NULL ||
        target_group->exec_in_progress)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-EINVAL;
    }

    tcb_t new_task = alloc_zeroed_tcb();
    if (new_task == NULL)
    {
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }

    new_task->task_level = TASK_KERNEL_LEVEL;
    new_task->cpu_id     = get_current_cpu()->processor_id;
    strncpy(new_task->name, name, sizeof(new_task->name) - 1);
    new_task->parent_group = target_group;

    uint64_t kernel_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (kernel_stack_phys == 0)
    {
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    uint64_t kernel_stack     = (uint64_t)phys_to_virt(kernel_stack_phys);
    new_task->context0.rflags = 0x202;
    new_task->context0.rip    = (uint64_t)_start;
    new_task->context0.rsp    = kernel_stack + KERNEL_STACK_SIZE; // 璁剧疆涓婁笅鏂?
    new_task->kernel_stack    = kernel_stack + KERNEL_STACK_SIZE; // 鏍?6瀛楄妭瀵归綈
    new_task->context0.cs     = 0x8;
    new_task->context0.ss     = 0x10;
    new_task->context0.es     = 0x10;
    new_task->context0.ds     = 0x10;
    new_task->fs              = 0x10;
    new_task->status          = CREATE;
    new_task->main            = (uint64_t)_start;
    save_fpu_context(&new_task->fpu_context);

    new_task->winnum  = 0;
    new_task->hasfscr = false;

    new_task->user_info = &root_user;

    uint64_t *stack_top = (uint64_t *)new_task->kernel_stack;
    PUSH_STACK(stack_top, 0);
    PUSH_STACK(stack_top, 0);
    PUSH_STACK(stack_top, (uint64_t)process_exit);
    new_task->context0.rsp = (uint64_t)stack_top;
    new_task->context0.rdi = (uint64_t)args; // first argument in rdi

    new_task->fs_base = (uint64_t)new_task;

    new_task->str_cwd = "/";
    new_task->cwd     = rootdir;
    new_task->window_count = 0;

    new_task->group_index = queue_enqueue(target_group->thread_queue, new_task);
    if (new_task->group_index == (size_t)-1)
    {
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    new_task->tid = alloc_tid();

    add_task(new_task);
    spin_unlock(&create_thread_lock);
    restore_runtime_state(was_scheduler_enabled, is_sti);
    return new_task->tid;
}


static size_t create_user_thread_internal(void *_start, void *args, int argc, char *name, pcb_t pcb, char *cwd,
                                          bool publish_task, tcb_t *task_out)
{
    if (task_out != NULL) *task_out = NULL;
    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    if (!no_interrupt) open_interrupt;

    spin_lock(&create_thread_lock);
    close_interrupt;
    disable_scheduler();
    if (_start == NULL || name == NULL || pcb == NULL || pcb->thread_queue == NULL || pcb->pagedir == NULL || !cwd ||
        pcb->exec_in_progress)
    {
        write_serial_fmt("You can't create an user thread WITHOUT A CWD \n");
        free(cwd);
        free_envp((char **)args);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-(EINVAL);
    }
    tcb_t new_task = alloc_zeroed_tcb();
    if (new_task == NULL)
    {
        free(cwd);
        free_envp((char **)args);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }

    new_task->task_level = TASK_APPLICATION_LEVEL;
    new_task->cpu_id     = get_current_cpu()->processor_id;
    strncpy(new_task->name, name, sizeof(new_task->name) - 1);
    new_task->parent_group = pcb;

    uint64_t kernel_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (kernel_stack_phys == 0)
    {
        free(cwd);
        free_envp((char **)args);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    uint64_t kernel_stack     = (uint64_t)phys_to_virt(kernel_stack_phys) + KERNEL_STACK_SIZE;
    uint64_t *stack_top       = (uint64_t *)kernel_stack;
    *(--stack_top)            = 0;
    *(--stack_top)            = 0;
    *(--stack_top)            = (uint64_t)switch_to_user_mode;
    new_task->context0.rflags = 0x202;
    new_task->context0.rip    = (uint64_t)switch_to_user_mode;
    new_task->context0.rsp    = (uint64_t)stack_top;
    new_task->kernel_stack    = kernel_stack;

    uint64_t syscall_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (syscall_stack_phys == 0)
    {
        free(cwd);
        free_envp((char **)args);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    new_task->syscall_stack = (uint64_t)phys_to_virt(syscall_stack_phys) + KERNEL_STACK_SIZE;

    new_task->cwd = vfs_open(cwd);
    if (new_task->cwd == NULL)
    {
        free(cwd);
        free_envp((char **)args);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOENT;
    }
    new_task->str_cwd = cwd;

    new_task->winnum  = 0;
    new_task->hasfscr = false;

    new_task->argv = (char **)args;
    new_task->argc = argc;

    new_task->user_info = current_user != NULL ? current_user : &root_user;

    if (pcb->prepared_user_rsp != 0)
    {
        new_task->user_stack = pcb->prepared_user_stack;
        new_task->user_stack_top = pcb->prepared_user_stack_top;
        new_task->owns_user_stack = true;
        new_task->uses_prepared_user_stack = true;
        new_task->prepared_user_rsp = pcb->prepared_user_rsp;
        new_task->prepared_user_argv = pcb->prepared_user_argv;
        new_task->prepared_user_envp = pcb->prepared_user_envp;
        new_task->prepared_user_entry_rdx = pcb->prepared_user_entry_rdx;
        pcb->prepared_user_rsp = 0;
    }
    else
    {
        new_task->user_stack = page_reserve_user_range(pcb->pagedir, BIG_USER_STACK);
        if (new_task->user_stack == 0)
        {
            vfs_close(new_task->cwd);
            free(cwd);
            free_envp((char **)args);
            free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
            free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
            free(new_task);
            spin_unlock(&create_thread_lock);
            restore_runtime_state(was_scheduler_enabled, is_sti);
            return (size_t)-ENOMEM;
        }
        new_task->user_stack_top = new_task->user_stack + BIG_USER_STACK;
        new_task->owns_user_stack = true;
    }

    new_task->main           = (uint64_t)_start;
    new_task->context0.cs    = 0x8;
    new_task->context0.ss = new_task->context0.es = new_task->context0.ds = 0x10;
    new_task->fs                                                   = 0;
    new_task->status                                                      = CREATE;
    new_task->eevdf_slice = 1000000ULL;
    save_fpu_context(&new_task->fpu_context);

    new_task->fs_base = 0;

    new_task->window_count = 0;

    new_task->group_index = queue_enqueue(pcb->thread_queue, new_task);
    if (new_task->group_index == (size_t)-1)
    {
        vfs_close(new_task->cwd);
        free(cwd);
        free_envp((char **)args);
        free_user_stack_mapping(new_task);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    new_task->tid = alloc_tid();

    if (task_out != NULL) *task_out = new_task;
    if (publish_task)
    {
        procfs_on_new_task(pcb);
        add_task(new_task);
    }
    spin_unlock(&create_thread_lock);
    restore_runtime_state(was_scheduler_enabled, is_sti);
    return new_task->tid;
}

size_t create_user_thread(void *_start, void *args, int argc, char *name, pcb_t pcb, char *cwd)
{
    return create_user_thread_internal(_start, args, argc, name, pcb, cwd, true, NULL);
}

size_t create_user_thread_unpublished(void *_start, void *args, int argc, char *name, pcb_t pcb, char *cwd,
                                      tcb_t *task_out)
{
    return create_user_thread_internal(_start, args, argc, name, pcb, cwd, false, task_out);
}

bool publish_user_thread(tcb_t task)
{
    if (task == NULL || task->parent_group == NULL || task->group_index == (size_t)-1) return false;
    procfs_on_new_task(task->parent_group);
    add_task(task);
    return true;
}

size_t create_message_thread(void *_start, char *name, pcb_t pcb, char *cwd, uint64_t arg)
{
    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    if (!no_interrupt) open_interrupt;

    tcb_t creator_task = get_current_task();

    spin_lock(&create_thread_lock);
    close_interrupt;
    disable_scheduler();
    if (_start == NULL || name == NULL || pcb == NULL || pcb->thread_queue == NULL || pcb->pagedir == NULL || !cwd ||
        pcb->exec_in_progress)
    {
        write_serial_fmt("You can't create an message thread WITHOUT A CWD \n");
        free(cwd);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-(EINVAL);
    }
    tcb_t new_task = alloc_zeroed_tcb();
    if (new_task == NULL)
    {
        free(cwd);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }

    new_task->task_level = TASK_APPLICATION_LEVEL;
    new_task->cpu_id     = get_current_cpu()->processor_id;
    strncpy(new_task->name, name, sizeof(new_task->name) - 1);
    new_task->parent_group = pcb;

    new_task->context0.rdi = arg;

    uint64_t kernel_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (kernel_stack_phys == 0)
    {
        free(cwd);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    uint64_t kernel_stack     = (uint64_t)phys_to_virt(kernel_stack_phys) + KERNEL_STACK_SIZE;
    uint64_t *stack_top       = (uint64_t *)kernel_stack;
    *(--stack_top)            = 0;
    *(--stack_top)            = 0;
    *(--stack_top)            = (uint64_t)jump_to_message;
    new_task->context0.rflags = 0x202;
    new_task->context0.rip    = (uint64_t)jump_to_message;
    new_task->context0.rsp    = (uint64_t)stack_top;
    new_task->kernel_stack    = kernel_stack;
    uint64_t syscall_stack_phys = alloc_frames(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (syscall_stack_phys == 0)
    {
        free(cwd);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    new_task->syscall_stack   = (uint64_t)phys_to_virt(syscall_stack_phys) + KERNEL_STACK_SIZE;

    new_task->cwd = vfs_open(cwd);
    if (new_task->cwd == NULL)
    {
        free(cwd);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOENT;
    }
    new_task->str_cwd = cwd;

    new_task->winnum  = 0;
    new_task->hasfscr = false;

    new_task->user_info = current_user;

    new_task->user_stack     = page_reserve_user_range(pcb->pagedir, BIG_USER_STACK);
    if (new_task->user_stack == 0)
    {
        vfs_close(new_task->cwd);
        free(cwd);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    new_task->user_stack_top = new_task->user_stack + BIG_USER_STACK;
    new_task->owns_user_stack = true;

    if (!ensure_message_entry_stub(pcb->pagedir))
    {
        vfs_close(new_task->cwd);
        free(cwd);
        free_user_stack_mapping(new_task);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        write_serial_string("Failed to prepare message thread entry stub.\n");
        return (size_t)-ENOMEM;
    }

    new_task->main           = (uint64_t)_start;
    new_task->context0.cs    = 0x8;
    new_task->context0.ss = new_task->context0.es = new_task->context0.ds = 0x10;
    new_task->fs                                                   = 0;
    new_task->status                                                      = CREATE;
    save_fpu_context(&new_task->fpu_context);

    // The user-mode message stub runs as a normal application thread. If it
    // inherits the default kernel-side TCB pointer as FSBASE, any libstdc++
    // or stack-canary access through %fs will fault in user mode.
    if (creator_task != NULL && creator_task->parent_group == pcb)
    {
        new_task->fs      = creator_task->fs;
        new_task->fs_base = creator_task->fs_base;
    }
    else
    {
        new_task->fs_base = (uint64_t)new_task;
    }

    new_task->window_count = 0;

    new_task->group_index = queue_enqueue(pcb->thread_queue, new_task);
    if (new_task->group_index == (size_t)-1)
    {
        vfs_close(new_task->cwd);
        free(cwd);
        free_user_stack_mapping(new_task);
        free_frames(syscall_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free_frames(kernel_stack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
        free(new_task);
        spin_unlock(&create_thread_lock);
        restore_runtime_state(was_scheduler_enabled, is_sti);
        return (size_t)-ENOMEM;
    }
    new_task->tid = alloc_tid();

    add_task(new_task);
    spin_unlock(&create_thread_lock);
    restore_runtime_state(was_scheduler_enabled, is_sti);
    return new_task->tid;
}

static pcb_t create_process_group_internal(const char *name, pcb_t parent, page_directory_t *directory, char *cmdline,
                                           bool publish)
{
    pcb_t pcb        = (pcb_t)calloc(1, sizeof(struct process_control_block));
    if (pcb == NULL) return NULL;

    pcb_t parent_group = parent == NULL ? kernel_group : parent;
    if (parent_group == NULL || parent_group->child_pcb == NULL)
    {
        free(pcb);
        return NULL;
    }

    pcb->pid         = alloc_pid();
    pcb->parent_task = parent_group;
    pcb->status      = START;
    pcb->task_level  = TASK_APPLICATION_LEVEL;
    pcb->notify_pcor_pipe_read_fd  = -1;
    pcb->notify_pcor_pipe_write_fd = -1;
    pcb->notify_pcor_tid           = 0;
    pcb->notify_pcor_func          = 0;
    pcb->notify_pcor_registered    = false;
    pcb->queue_index = (size_t)-1;
    pcb->child_index = (size_t)-1;
    if (publish)
    {
        pcb->queue_index = queue_enqueue(pcb_group_queue, pcb);
        if (pcb->queue_index == (size_t)-1)
        {
            free(pcb);
            return NULL;
        }
    }
    strncpy(pcb->name, name != NULL ? name : "", sizeof(pcb->name) - 1);
    pcb->thread_queue = queue_init();
    pcb->ppid         = 0;
    pcb->pagedir      = directory ? directory : get_kernel_pagedir();
    pcb->child_pcb    = queue_init();
    pcb->vfork        = false;
    pcb->file_open    = queue_init();
    pcb->file_open_shared_refs = alloc_fd_table_ref();
    pcb->virt_queue   = queue_init();
    pcb->ipc_queue    = queue_init();
    pcb->tty          = alloc_default_tty();
    if (pcb->thread_queue == NULL || pcb->child_pcb == NULL || pcb->file_open == NULL ||
        pcb->file_open_shared_refs == NULL || pcb->virt_queue == NULL || pcb->ipc_queue == NULL || pcb->tty == NULL)
    {
        if (pcb->thread_queue) queue_destroy(pcb->thread_queue);
        if (pcb->child_pcb) queue_destroy(pcb->child_pcb);
        if (pcb->file_open) queue_destroy(pcb->file_open);
        free(pcb->file_open_shared_refs);
        if (pcb->virt_queue) queue_destroy(pcb->virt_queue);
        if (pcb->ipc_queue) queue_destroy(pcb->ipc_queue);
        free_tty(pcb->tty);
        if (publish) queue_remove_at(pcb_group_queue, pcb->queue_index);
        free(pcb);
        return NULL;
    }

    if (parent == NULL)
    {
        UserInfo *user = current_user != NULL ? current_user : &root_user;
        pcb->envc = user->envc;
        pcb->envp = copy_envp(user->envp);
    }
    else
    {
        UserInfo *user = current_user != NULL ? current_user : &root_user;
        pcb->envc = parent->envp == NULL ? user->envc : parent->envc;
        pcb->envp = copy_envp(parent->envp == NULL ? user->envp : parent->envp);
    }
    if (pcb->envp == NULL)
    {
        queue_destroy(pcb->thread_queue);
        queue_destroy(pcb->child_pcb);
        queue_destroy(pcb->file_open);
        free(pcb->file_open_shared_refs);
        queue_destroy(pcb->virt_queue);
        queue_destroy(pcb->ipc_queue);
        free_tty(pcb->tty);
        if (publish) queue_remove_at(pcb_group_queue, pcb->queue_index);
        free(pcb);
        return NULL;
    }

    pcb->cmdline     = strdup(cmdline != NULL ? cmdline : "");
    if (pcb->cmdline == NULL)
    {
        free_envp(pcb->envp);
        queue_destroy(pcb->thread_queue);
        queue_destroy(pcb->child_pcb);
        queue_destroy(pcb->file_open);
        free(pcb->file_open_shared_refs);
        queue_destroy(pcb->virt_queue);
        queue_destroy(pcb->ipc_queue);
        free_tty(pcb->tty);
        if (publish) queue_remove_at(pcb_group_queue, pcb->queue_index);
        free(pcb);
        return NULL;
    }
    if (publish)
    {
        pcb->child_index = queue_enqueue(pcb->parent_task->child_pcb, pcb);
        if (pcb->child_index == (size_t)-1)
        {
            free(pcb->cmdline);
            free_envp(pcb->envp);
            queue_destroy(pcb->thread_queue);
            queue_destroy(pcb->child_pcb);
            queue_destroy(pcb->file_open);
            free(pcb->file_open_shared_refs);
            queue_destroy(pcb->virt_queue);
            queue_destroy(pcb->ipc_queue);
            free_tty(pcb->tty);
            queue_remove_at(pcb_group_queue, pcb->queue_index);
            free(pcb);
            return NULL;
        }
    }

    pcb->brk_start   = USER_BRK_START;
    pcb->brk_end     = USER_BRK_END;
    pcb->brk_current = pcb->brk_start;
    pcb->mmap_start  = USER_MMAP_START;
    // pcb->msgprci = false;

    pcb->xtttp_stc = (xtttp_dtt *)malloc(sizeof(xtttp_dtt));
    if (pcb->xtttp_stc == NULL)
    {
        if (publish) queue_remove_at(pcb->parent_task->child_pcb, pcb->child_index);
        free(pcb->cmdline);
        free_envp(pcb->envp);
        queue_destroy(pcb->thread_queue);
        queue_destroy(pcb->child_pcb);
        queue_destroy(pcb->file_open);
        queue_destroy(pcb->virt_queue);
        queue_destroy(pcb->ipc_queue);
        free_tty(pcb->tty);
        if (publish) queue_remove_at(pcb_group_queue, pcb->queue_index);
        free(pcb);
        return NULL;
    }
    memset(pcb->xtttp_stc, 0, sizeof(xtttp_dtt));

    return pcb;
}

pcb_t create_process_group(const char *name, pcb_t parent, page_directory_t *directory, char *cmdline)
{
    return create_process_group_internal(name, parent, directory, cmdline, true);
}

pcb_t create_process_group_unpublished(const char *name, pcb_t parent, page_directory_t *directory, char *cmdline)
{
    return create_process_group_internal(name, parent, directory, cmdline, false);
}

bool publish_process_group(pcb_t pcb)
{
    if (pcb == NULL || pcb->parent_task == NULL || pcb->queue_index != (size_t)-1 || pcb->child_index != (size_t)-1)
        return false;

    pcb->queue_index = queue_enqueue(pcb_group_queue, pcb);
    if (pcb->queue_index == (size_t)-1) return false;
    pcb->child_index = queue_enqueue(pcb->parent_task->child_pcb, pcb);
    if (pcb->child_index == (size_t)-1)
    {
        queue_remove_at(pcb_group_queue, pcb->queue_index);
        pcb->queue_index = (size_t)-1;
        return false;
    }
    return true;
}

extern "C" uint64_t switch_to_kernel_stack()
{
    return get_current_task()->syscall_stack;
}

bool       smp_scheduler_lock = true;
extern int scheduler_is_ready;

void process_setup()
{
    user_image_retirement_queue_init(&execve_retirement_queue);
    pcb_group_queue     = queue_init();
    pcb_t kernel_pcb    = (pcb_t)calloc(1, sizeof(struct process_control_block));
    kernel_pcb->pid     = alloc_pid();
    kernel_pcb->pagedir = get_kernel_pagedir();
    strcpy(kernel_pcb->name, "XSK System");
    kernel_pcb->parent_task  = kernel_pcb;
    kernel_pcb->status       = RUNNING;
    kernel_pcb->queue_index  = queue_enqueue(pcb_group_queue, kernel_pcb);
    kernel_pcb->thread_queue = queue_init();
    kernel_pcb->child_pcb    = queue_init();
    kernel_pcb->xtttp_stc    = (xtttp_dtt *)calloc(1,sizeof(xtttp_dtt));
    kernel_pcb->file_open    = queue_init();
    kernel_pcb->file_open_shared_refs = alloc_fd_table_ref();
    kernel_pcb->virt_queue   = queue_init();
    kernel_pcb->ipc_queue    = queue_init();
    kernel_pcb->mmap_start   = USER_MMAP_START;

    kernel_pcb->child_index = 0;

    kernel_group = kernel_pcb;

    smp_scheduler_lock = false;
    scheduler_is_ready++;

    write_serial_fmt("Setup process <%s> PID:%zu\n", kernel_pcb->name, kernel_pcb->pid);
}
EXPORT_SYMBOL(create_kernel_thread);
