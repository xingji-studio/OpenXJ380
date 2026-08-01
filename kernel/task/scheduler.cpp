#include "cpu/fpu.h"
#include "cpu/fsgsbase.h"
#include "cpu/lock.h"
#include "dlinker.h"
#include "krlibc.h"
#include "lock_queue.h"
#include "proto.hpp"
#include "smp/smp.h"
#include "task/pcb.h"

volatile bool        is_scheduler = false;
extern lock_queue   *pcb_group_queue;
extern XSK_SMP_INFO *xsi;
extern bool          no_interrupt;
const uint64_t       TIME_SLICE = 4;
const uint64_t       MIN_SLICE  = 1;
static constexpr uint64_t EEVDF_TICK_NS        = 1000000ULL;
static constexpr uint64_t EEVDF_BASE_SLICE_NS  = TIME_SLICE * EEVDF_TICK_NS;
static constexpr uint64_t EEVDF_MIN_SLICE_NS   = MIN_SLICE * EEVDF_TICK_NS;
static constexpr uint64_t EEVDF_WAKEUP_CREDIT  = EEVDF_BASE_SLICE_NS;
static constexpr uint64_t EEVDF_SLEEPER_CREDIT = EEVDF_BASE_SLICE_NS * 2;
static constexpr uint64_t EEVDF_DEFAULT_WEIGHT = 1024ULL;

void enable_scheduler()
{
    is_scheduler = true;
}

void disable_scheduler()
{
    is_scheduler = false;
}

tcb_t get_current_task()
{
    return get_current_cpu()->current_task;
}
EXPORT_SYMBOL(get_current_task);

__attribute__((naked)) void save_registers()
{
    __asm__ volatile(".intel_syntax noprefix\n\t"
                     "cli\n\t"
                     "push 0\n\t" // 对齐
                     "push 0\n\t" // 对齐
                     "push r15\n\t"
                     "push r14\n\t"
                     "push r13\n\t"
                     "push r12\n\t"
                     "push r11\n\t"
                     "push r10\n\t"
                     "push r9\n\t"
                     "push r8\n\t"
                     "push rdi\n\t"
                     "push rsi\n\t"
                     "push rbp\n\t"
                     "push rdx\n\t"
                     "push rcx\n\t"
                     "push rbx\n\t"
                     "push rax\n\t"
                     "mov rax, es\n\t"
                     "push rax\n\t"
                     "mov rax, ds\n\t"
                     "push rax\n\t"
                     "mov rdi, rsp\n\t"
                     "call timer_handle\n\t"
                     "mov rsp, rax\n\t"
                     "pop rax\n\t"
                     "mov ds, rax\n\t"
                     "pop rax\n\t"
                     "mov es, rax\n\t"
                     "pop rax\n\t"
                     "pop rbx\n\t"
                     "pop rcx\n\t"
                     "pop rdx\n\t"
                     "pop rbp\n\t"
                     "pop rsi\n\t"
                     "pop rdi\n\t"
                     "pop r8\n\t"
                     "pop r9\n\t"
                     "pop r10\n\t"
                     "pop r11\n\t"
                     "pop r12\n\t"
                     "pop r13\n\t"
                     "pop r14\n\t"
                     "pop r15\n\t"
                     "add rsp, 16\n\t" // 越过对齐
                     // Let iretq restore IF from the saved RFLAGS. Forcing sti here
                     // can re-enter interrupts before the full return frame is restored.
                     "iretq\n\t");
}

void change_proccess(registers_t *reg, tcb_t current_task0, tcb_t target)
{
    current_task0->fs_base = read_fsbase();

    switch_page_directory(target->parent_group->pagedir);
    set_kernel_stack(target->kernel_stack);
    write_kgsbase((uint64_t)get_current_cpu());

    if ((target->context0.cs & 0x3) == 0x3) {
        // Returning to user mode: keep user GSBASE clear and leave CPU-local
        // data in KGSBASE for the next syscall/swapgs entry.
        write_gsbase(0);
        __asm__ __volatile__("movq %0, %%fs\n\t" ::"r"(target->fs));
    } else {
        // Returning to kernel mode (for example, resuming inside a syscall):
        // kernel code expects %gs to point at the current CPU area.
        uint64_t fs_selector = target->fs;
        if (target->task_level != TASK_APPLICATION_LEVEL && fs_selector == 0)
        {
            fs_selector = 0x10;
        }
        write_gsbase((uint64_t)get_current_cpu());
        __asm__ __volatile__("movq %0, %%fs\n\t" ::"r"(fs_selector));
    }
    write_fsbase(target->fs_base);

    save_fpu_context(&current_task0->fpu_context);
    restore_fpu_context(&target->fpu_context);

    current_task0->context0.r15    = reg->r15;
    current_task0->context0.r14    = reg->r14;
    current_task0->context0.r13    = reg->r13;
    current_task0->context0.r12    = reg->r12;
    current_task0->context0.r11    = reg->r11;
    current_task0->context0.r10    = reg->r10;
    current_task0->context0.r9     = reg->r9;
    current_task0->context0.r8     = reg->r8;
    current_task0->context0.rax    = reg->rax;
    current_task0->context0.rbx    = reg->rbx;
    current_task0->context0.rcx    = reg->rcx;
    current_task0->context0.rdx    = reg->rdx;
    current_task0->context0.rdi    = reg->rdi;
    current_task0->context0.rsi    = reg->rsi;
    current_task0->context0.rbp    = reg->rbp;
    current_task0->context0.rflags = reg->rflags;
    current_task0->context0.rip    = reg->rip;
    current_task0->context0.rsp    = reg->rsp;
    current_task0->context0.ss     = reg->ss;
    current_task0->context0.es     = reg->es;
    current_task0->context0.cs     = reg->cs;
    current_task0->context0.ds     = reg->ds;

    reg->r15    = target->context0.r15;
    reg->r14    = target->context0.r14;
    reg->r13    = target->context0.r13;
    reg->r12    = target->context0.r12;
    reg->r11    = target->context0.r11;
    reg->r10    = target->context0.r10;
    reg->r9     = target->context0.r9;
    reg->r8     = target->context0.r8;
    reg->rax    = target->context0.rax;
    reg->rbx    = target->context0.rbx;
    reg->rcx    = target->context0.rcx;
    reg->rdx    = target->context0.rdx;
    reg->rdi    = target->context0.rdi;
    reg->rsi    = target->context0.rsi;
    reg->rbp    = target->context0.rbp;
    reg->rflags = target->context0.rflags;
    reg->rip    = target->context0.rip;
    reg->rsp    = target->context0.rsp;
    reg->ss     = target->context0.ss;
    reg->es     = target->context0.es;
    reg->ds     = target->context0.ds;
    reg->cs     = target->context0.cs;
}

spin_t scheduler_lock = SPIN_INIT;

static inline bool is_task_schedulable(tcb_t task, tcb_t current)
{
    if (task == NULL) return false;
    if (task == current) return false;
    if (task->status != RUNNING && task->status != START && task->status != CREATE) return false;
    if (task->task_level == TASK_IDLE_LEVEL) return false;
    if (task->parent_group == NULL) return false;
    if (task->parent_group->status == DEATH || task->parent_group->status == FUTEX ||
        task->parent_group->status == OUT || task->parent_group->status == WAIT) {
        return false;
    }
    return true;
}

static inline bool is_current_task_runnable(tcb_t task)
{
    if (task == NULL) return false;
    if (task->status != RUNNING && task->status != START && task->status != CREATE) return false;
    if (task->parent_group == NULL) return false;
    if (task->parent_group->status == DEATH || task->parent_group->status == FUTEX ||
        task->parent_group->status == OUT || task->parent_group->status == WAIT) {
        return false;
    }
    return true;
}

static inline uint64_t task_sched_slice(tcb_t task)
{
    if (task == NULL || task->eevdf_slice < EEVDF_MIN_SLICE_NS) return EEVDF_BASE_SLICE_NS;
    return task->eevdf_slice;
}

static inline void apply_eevdf_wakeup_credit(tcb_t task, uint64_t base_vruntime)
{
    if (task == NULL || task->task_level == TASK_IDLE_LEVEL) return;

    uint64_t credit = task_sched_slice(task);
    if (credit < EEVDF_WAKEUP_CREDIT) credit = EEVDF_WAKEUP_CREDIT;
    if (credit > EEVDF_SLEEPER_CREDIT) credit = EEVDF_SLEEPER_CREDIT;

    uint64_t placed_vruntime = base_vruntime > credit ? base_vruntime - credit : 0;
    if (task->eevdf_vruntime > placed_vruntime) {
        task->eevdf_vruntime = placed_vruntime;
    }
    task->eevdf_deadline = task->eevdf_vruntime + task_sched_slice(task);
}

static inline bool wake_sleeping_task(tcb_t task, uint64_t now, uint64_t base_vruntime)
{
    if (task == NULL) return false;
    if (task->status == WAIT && task->wakeup_time != 0 && now >= task->wakeup_time) {
        task->wakeup_time = 0;
        task->status = START;
        apply_eevdf_wakeup_credit(task, base_vruntime);
        return true;
    }
    return false;
}

static inline uint64_t vruntime_delta(uint64_t runtime_ns)
{
    return (runtime_ns * EEVDF_DEFAULT_WEIGHT) / EEVDF_DEFAULT_WEIGHT;
}

static inline bool deadline_before(tcb_t left, tcb_t right)
{
    if (right == NULL) return true;
    if (left == NULL) return false;
    if (left->eevdf_deadline != right->eevdf_deadline) return left->eevdf_deadline < right->eevdf_deadline;
    return left->eevdf_vruntime < right->eevdf_vruntime;
}

static void charge_current_eevdf_runtime(tcb_t current, uint64_t runtime_ns)
{
    if (current == NULL || current->task_level == TASK_IDLE_LEVEL) return;
    if (current->status != RUNNING) return;
    if (runtime_ns == 0) return;

    current->eevdf_vruntime += vruntime_delta(runtime_ns);
    uint64_t slice = task_sched_slice(current);
    current->eevdf_deadline = current->eevdf_vruntime + slice;
}

static uint64_t queue_average_vruntime(lock_queue *queue, tcb_t current, uint64_t now, tcb_t *fallback, tcb_t *idle)
{
    uint64_t sum = 0;
    uint64_t count = 0;
    if (fallback != NULL) *fallback = NULL;
    if (idle != NULL) *idle = NULL;

    lock_node *node = queue->head;
    while (node != NULL) {
        tcb_t candidate = (tcb_t)node->data;
        uint64_t wake_base = current != NULL ? current->eevdf_vruntime : candidate->eevdf_vruntime;
        wake_sleeping_task(candidate, now, wake_base);
        if (candidate != current && candidate != NULL && candidate->task_level == TASK_IDLE_LEVEL && idle != NULL) {
            *idle = candidate;
        }
        if (candidate == current) {
            if (is_current_task_runnable(candidate)) {
                sum += candidate->eevdf_vruntime;
                count++;
            }
        } else if (is_task_schedulable(candidate, current)) {
            sum += candidate->eevdf_vruntime;
            count++;
            if (fallback != NULL && deadline_before(candidate, *fallback)) {
                *fallback = candidate;
            }
        }
        node = node->next;
    }

    return count == 0 ? 0 : sum / count;
}

static void init_task_eevdf_entity(tcb_t task, uint64_t base_vruntime, uint64_t now)
{
    if (task == NULL) return;
    uint64_t slice = task_sched_slice(task);
    task->eevdf_slice = slice;
    task->eevdf_vruntime = base_vruntime > EEVDF_WAKEUP_CREDIT ? base_vruntime - EEVDF_WAKEUP_CREDIT : 0;
    task->eevdf_deadline = task->eevdf_vruntime + slice;
    task->eevdf_last_start = now;
}

void scheduler_init_task(tcb_t task)
{
    init_task_eevdf_entity(task, 0, nanoTime());
}

static void mark_task_dispatched(tcb_t task, uint64_t now)
{
    if (task == NULL) return;
    if (task->eevdf_slice < EEVDF_MIN_SLICE_NS) {
        task->eevdf_slice = EEVDF_BASE_SLICE_NS;
    }
    if (task->eevdf_deadline == 0) {
        task->eevdf_deadline = task->eevdf_vruntime + task_sched_slice(task);
    }
    task->eevdf_last_start = now;
}

tcb_t select_next_task_safe()
{
    struct PROCESSOR_INFO *cpu = get_current_cpu();
    lock_queue *queue = cpu->scheduler_queue;
    
    if (queue == NULL || queue->size == 0) {
        return NULL;
    }

    spin_lock(&queue->lock);
    
    tcb_t current = get_current_task();
    uint64_t now = nanoTime();
    wake_sleeping_task(current, now, current != NULL ? current->eevdf_vruntime : 0);
    if (queue->head == NULL) {
        spin_unlock(&queue->lock);
        return NULL;
    }

    tcb_t fallback = NULL;
    tcb_t idle = NULL;
    const uint64_t avg_vruntime = queue_average_vruntime(queue, current, now, &fallback, &idle);
    tcb_t best = NULL;

    if (fallback != NULL && fallback->eevdf_vruntime <= avg_vruntime) {
        best = fallback;
    }

    if (best == NULL && fallback != NULL) {
        lock_node *node = queue->head;
        while (node != NULL) {
            tcb_t candidate = (tcb_t)node->data;
            if (is_task_schedulable(candidate, current) && candidate->eevdf_vruntime <= avg_vruntime &&
                deadline_before(candidate, best)) {
                    best = candidate;
            }
            node = node->next;
        }
    }

    if (best == NULL) best = fallback;

    tcb_t result = best != NULL ? best : (is_current_task_runnable(current) ? current : idle);
    if (result != NULL) mark_task_dispatched(result, now);
    spin_unlock(&queue->lock);
    return result;
}

tcb_t select_next_task()
{
    tcb_t next = select_next_task_safe();
    
    // 如果没有找到合适的任务，返回当前任务（如果可运行）
    if (next == NULL) {
        tcb_t current = get_current_task();
        if (current && (current->status == RUNNING || current->status == START)) {
            return current;
        }
        return NULL;
    }
    
    return next;
}

extern pcb_t kernel_group;
extern "C" registers_t *timer_handle(registers_t *reg)
{
    // send_eoi();
    if (!is_scheduler) { 
        send_eoi();
        return reg; 
    }

    tcb_t current = get_current_task();
    if (current == NULL) {
        send_eoi();
        return reg;
    }

    PROCESSOR_INFO *cpu = get_current_cpu();
    if (likely(current->status == RUNNING && current->task_level != TASK_IDLE_LEVEL)) {
        cpu->scheduler_ticks++;
        charge_current_eevdf_runtime(current, EEVDF_TICK_NS);
        if (cpu->scheduler_ticks < TIME_SLICE) {
            send_eoi();
            return reg;
        }
    } else {
        cpu->scheduler_ticks = 0;
    }

    tcb_t best = select_next_task();
    if (best == NULL || best == current) {
        cpu->scheduler_ticks = 0;
        send_eoi();
        return reg;
    }

    // 记录当前线程运行时最新的 FSBASE，避免调度后丢失用户态 TLS 环境。
    current->fs_base = read_fsbase();

    // 任务寻父处理
    if (best->parent_group && 
        (best->parent_group->parent_task == NULL || 
         best->parent_group->parent_task->status == DEATH || 
         best->parent_group->parent_task->status == OUT)) {
        best->parent_group->parent_task = kernel_group;
    }

    // 正式切换
    if (get_current_cpu()->current_task != best) {
        disable_scheduler();
        
        // 更新任务状态
        if (current->status == RUNNING) { 
            current->status = START; 
        }
        if (best->status == START || best->status == CREATE) { 
            best->status = RUNNING; 
        }
        
        // 只有上下文有效的任务才切换
        if (best->context0.rip != 0) {
            cpu->scheduler_ticks = 0;
            change_proccess(reg, current, best);
            cpu->current_task = best;
        } else {
            // 如果上下文无效，不切换并恢复状态
            current->status = RUNNING;
            cpu->scheduler_ticks = 0;
        }
        
        enable_scheduler();
    }
    send_eoi();
    return reg;
}

void scheduler_yield()
{
    if (!is_scheduler) return;
    get_current_cpu()->scheduler_ticks = TIME_SLICE;
    __asm__ volatile("int %0" ::"i"(32));
}
EXPORT_SYMBOL(scheduler_yield);

void scheduler_sleep_ns(uint64_t nano)
{
    if (!is_scheduler || nano == 0) {
        scheduler_yield();
        return;
    }

    tcb_t current = get_current_task();
    if (current == NULL || current->task_level == TASK_IDLE_LEVEL) {
        scheduler_yield();
        return;
    }

    uint64_t now = nanoTime();
    uint64_t wakeup_time = now + nano;
    if (wakeup_time < now) wakeup_time = (uint64_t)-1;

    current->wakeup_time = wakeup_time;
    current->status = WAIT;
    do {
        scheduler_yield();
    } while (current->status == WAIT);

    if (current->status == START) current->status = RUNNING;
}
EXPORT_SYMBOL(scheduler_sleep_ns);

void scheduler_wake_task(tcb_t task)
{
    if (task == NULL || task->status != WAIT) return;

    uint64_t base_vruntime = 0;
    tcb_t current = get_current_task();
    if (current != NULL && current->task_level != TASK_IDLE_LEVEL) {
        base_vruntime = current->eevdf_vruntime;
    }

    task->wakeup_time = 0;
    task->status = START;
    apply_eevdf_wakeup_credit(task, base_vruntime);
}
EXPORT_SYMBOL(scheduler_wake_task);

void debug_sched_queue(lock_queue *q)
{
    if (q == NULL) return;
    spin_lock(&q->lock);
    lock_node *current = q->head;
    write_serial_fmt("Process %d Scheduler Queue: ", get_current_cpu()->lapic_id);
    while (current != NULL)
    {
        write_serial_fmt("-> %s", ((tcb_t)current->data)->name);
        current = current->next;
    }
    write_serial_string("\n");
    spin_unlock(&q->lock);
}

size_t add_task(tcb_t new_task)
{
    if (new_task == NULL) return -1;
	    
    spin_lock(&scheduler_lock);
    
    // 确保任务有基本设置
    if (new_task->status == CREATE) {
        new_task->status = START;
    }
    
    struct PROCESSOR_INFO *min_cpu = get_cpu(0);
    size_t                 min_cpu_index = 0;

    if (new_task->task_level != TASK_APPLICATION_LEVEL) {
        for (size_t i = 1; i < get_cpu_num(); i++) {
            struct PROCESSOR_INFO *cpui = get_cpu(i);
            if (cpui != NULL && cpui->scheduler_queue != NULL &&
                cpui->scheduler_queue->size < min_cpu->scheduler_queue->size) {
                min_cpu       = cpui;
                min_cpu_index = i;
            }
        }
    }
	    
    uint64_t now = nanoTime();
    init_task_eevdf_entity(new_task, queue_average_vruntime(min_cpu->scheduler_queue, NULL, now, NULL, NULL), now);
    new_task->cpu_id      = min_cpu_index;
    new_task->queue_index = queue_enqueue_ref(min_cpu->scheduler_queue, new_task, &new_task->sched_node);

    // debug_sched_queue(min_cpu->scheduler_queue);
    
    spin_unlock(&scheduler_lock);
    return new_task->queue_index;
}

void remove_task(tcb_t task) {
    if (task == NULL) return;
    spin_lock(&scheduler_lock);

    if (task->cpu_id < get_cpu_num()) {
        PROCESSOR_INFO *cpu = get_cpu(task->cpu_id);
        if (cpu != NULL &&
            cpu->scheduler_queue != NULL &&
            task->sched_node != NULL &&
            task->sched_node->data == task) {
            queue_remove_node(cpu->scheduler_queue, task->sched_node);
            task->sched_node = NULL;
            spin_unlock(&scheduler_lock);
            return;
        }
    }

    for (size_t i = 0; i < get_cpu_num(); i++) {
        PROCESSOR_INFO *cpu = get_cpu(i);
        if (cpu == NULL || cpu->scheduler_queue == NULL) continue;
        lock_node *node = cpu->scheduler_queue->head;
        while (node != NULL && node->data != task) node = node->next;
        if (node == NULL) continue;

        task->cpu_id = i;
        task->sched_node = node;
        queue_remove_node(cpu->scheduler_queue, task->sched_node);
        task->sched_node = NULL;
        break;
    }

    spin_unlock(&scheduler_lock);
}
