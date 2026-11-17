#include <proto.hpp>
#include <cpu/longm.h>
#include <mm/uaccess.h>
#include <syscall/signal.h>
#include <task/pcb.h>

struct user_signal_frame {
    uint64_t magic;
    int      sig;
    sigset_t oldmask;
    struct X64_REGS regs;
};

static constexpr uint64_t SIGNAL_FRAME_MAGIC = 0x583338305349474eULL; // X380SIGN

signal_internal_t signal_internal_decisions[65];

namespace
{
// Linux x86-64's kernel ucontext contains an eight-byte sigset, followed by
// siginfo. libc's larger sigset/ucontext types retain this common ABI prefix.
struct LinuxUcontext
{
    uint64_t flags, link;
    stack_t stack;
    struct sigcontext machine;
    uint64_t mask;
};

struct LinuxSiginfo
{
    int32_t number, error, code, padding;
    uint64_t address;
    uint8_t reserved[104];
};

struct LinuxSignalFrame
{
    uint64_t restorer;
    LinuxUcontext context;
    LinuxSiginfo info;
};

static_assert(sizeof(struct sigcontext) == 256, "Linux sigcontext ABI");
static_assert(sizeof(LinuxUcontext) == 304, "Linux ucontext ABI");
static_assert(sizeof(LinuxSiginfo) == 128, "Linux siginfo ABI");
static_assert(sizeof(LinuxSignalFrame) == 440, "Linux rt_sigframe ABI");

static bool linux_task(tcb_t task)
{
    return task && task->parent_group && task->parent_group->linux_abi;
}

static uint64_t signal_bit(tcb_t task, int sig)
{
    return 1ULL << (linux_task(task) ? sig - 1 : sig);
}

static int last_signal(tcb_t task)
{
    return linux_task(task) ? 64 : MAXSIG;
}

static sighandler_t handler_for(tcb_t task, int sig)
{
    sigaction_t *action = linux_task(task) ? &task->parent_group->linux_actions[sig - 1] : &task->actions[sig];
    return __atomic_load_n(&action->sa_handler, __ATOMIC_ACQUIRE);
}

static sigaction_t action_for(tcb_t task, int sig)
{
    if (!linux_task(task)) return task->actions[sig];
    pcb_t process = task->parent_group;
    spin_lock(&process->linux_signal_lock);
    sigaction_t action = process->linux_actions[sig - 1];
    spin_unlock(&process->linux_signal_lock);
    return action;
}

static uint64_t unmaskable(tcb_t task)
{
    return signal_bit(task, SIGKILL) | signal_bit(task, SIGSTOP);
}

static void *entry_fpstate(X64_REGS *regs)
{
    // Both Err_Code and syscall_entry in handler.S save a private FXSAVE64
    // image immediately below this unchanged GPR frame, aligned down to 16.
    return (void *)(((uint64_t)regs - 512) & ~15ULL);
}
}

static bool signal_copy_to_user(page_directory_t *pagedir, void *dst, const void *src, size_t size)
{
    if (size == 0) return true;
    if (src == NULL) return false;
    return copy_to_user_pagedir(pagedir, dst, src, size);
}

static bool signal_copy_from_user(page_directory_t *pagedir, void *dst, const void *src, size_t size)
{
    if (size == 0) return true;
    if (dst == NULL) return false;
    return copy_from_user_pagedir(pagedir, dst, src, size);
}

bool signals_pending_quick(tcb_t task) {
    if (task == NULL) return false;
    // Protected syscall owners remain live until their resources unwind.
    // Their waits must observe cancellation even without a Linux signal.
    if (__atomic_load_n(&task->kernel_exit_pending, __ATOMIC_ACQUIRE)) return true;
    sigset_t pending_list   = __atomic_load_n(&task->signal, __ATOMIC_ACQUIRE);
    sigset_t unblocked_list = pending_list & (~task->blocked);
    for (int i = MINSIG; i <= last_signal(task); i++) {
        if (!(unblocked_list & signal_bit(task, i))) continue;
        sighandler_t user_handler = handler_for(task, i);
        if (user_handler == SIG_IGN) continue;
        if (user_handler == SIG_DFL && signal_internal_decisions[i] == SIGNAL_INTERNAL_IGN)
            continue;

        return true;
    }
    return false;
}

int signal_setmask(int how, const sigset_t *nset, sigset_t *oset)
{
    tcb_t task = get_current_task();
    if (task == NULL) return -ESRCH;

    if (oset) *oset = task->blocked;
    if (nset) {
        uint64_t safe = *nset;
        safe         &= ~unmaskable(task);
        switch (how) {
        case SIG_BLOCK: task->blocked |= safe; break;
        case SIG_UNBLOCK: task->blocked &= ~(safe); break;
        case SIG_SETMASK: task->blocked = safe; break;
        default: return -EINVAL;
        }
    }
    return EOK;
}

int syscall_ssetmask(int how, sigset_t *nset, sigset_t *oset) {
    tcb_t task = get_current_task();
    if (task == NULL) return -ESRCH;

    page_directory_t *pagedir = task->parent_group ? task->parent_group->pagedir : NULL;

    sigset_t oldset = 0;
    sigset_t newset = 0;
    if (nset) {
        if (!signal_copy_from_user(pagedir, &newset, nset, sizeof(newset))) return -EFAULT;
    }

    int ret = signal_setmask(how, nset ? &newset : NULL, oset ? &oldset : NULL);
    if (ret < 0) return ret;
    if (oset && !signal_copy_to_user(pagedir, oset, &oldset, sizeof(oldset))) return -EFAULT;
    return EOK;
}

int syscall_sig_action(int sig, sigaction_t *action, sigaction_t *oldaction) {
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return -ESRCH;
    if (sig < MINSIG || sig > last_signal(task) || sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    page_directory_t *pagedir = task->parent_group->pagedir;
    sigaction_t requested, previous;
    if (action && !signal_copy_from_user(pagedir, &requested, action, sizeof(requested))) return -EFAULT;
    bool linux_abi = linux_task(task);
    sigaction_t *ptr = linux_abi ? &task->parent_group->linux_actions[sig - 1] : &task->actions[sig];
    if (linux_abi) spin_lock(&task->parent_group->linux_signal_lock);
    previous = *ptr;
    if (action)
    {
        if (!linux_abi)
        {
            if (requested.sa_flags & SIG_NOMASK) requested.sa_mask = 0;
            else requested.sa_mask |= signal_bit(task, sig);
        }
        ptr->sa_flags = requested.sa_flags;
        ptr->sa_restorer = requested.sa_restorer;
        ptr->sa_mask = requested.sa_mask;
        __atomic_store_n(&ptr->sa_handler, requested.sa_handler, __ATOMIC_RELEASE);
    }
    if (linux_abi) spin_unlock(&task->parent_group->linux_signal_lock);
    if (oldaction && !signal_copy_to_user(pagedir, oldaction, &previous, sizeof(previous))) return -EFAULT;
    return 0;
}

void signal_fork_process(pcb_t child, pcb_t parent)
{
    spin_lock(&parent->linux_signal_lock);
    memcpy(child->linux_actions, parent->linux_actions, sizeof(child->linux_actions));
    spin_unlock(&parent->linux_signal_lock);
}

void signal_exec_process(pcb_t process)
{
    spin_lock(&process->linux_signal_lock);
    for (sigaction_t &action : process->linux_actions)
        if (action.sa_handler != SIG_IGN)
        {
            action.sa_flags = 0;
            action.sa_restorer = NULL;
            action.sa_mask = 0;
            __atomic_store_n(&action.sa_handler, SIG_DFL, __ATOMIC_RELEASE);
        }
    spin_unlock(&process->linux_signal_lock);
}

int syscall_sigaltstack(stack_t *ss, stack_t *old_ss)
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return -ESRCH;

    page_directory_t *pagedir = task->parent_group->pagedir;
    if (old_ss != NULL)
    {
        stack_t current;
        current.ss_sp    = task->sas_ss_sp;
        current.ss_flags = task->sas_ss_flags;
        current.ss_size  = task->sas_ss_size;
        if (!signal_copy_to_user(pagedir, old_ss, &current, sizeof(current))) return -EFAULT;
    }

    if (ss != NULL)
    {
        stack_t requested;
        if (!signal_copy_from_user(pagedir, &requested, ss, sizeof(requested))) return -EFAULT;
        if ((requested.ss_flags & ~(SS_DISABLE)) != 0) return -EINVAL;

        if (requested.ss_flags & SS_DISABLE)
        {
            task->sas_ss_sp    = NULL;
            task->sas_ss_size  = 0;
            task->sas_ss_flags = SS_DISABLE;
        }
        else
        {
            if (requested.ss_size < MINSIGSTKSZ) return -ENOMEM;
            task->sas_ss_sp    = requested.ss_sp;
            task->sas_ss_size  = requested.ss_size;
            task->sas_ss_flags = 0;
        }
    }

    return 0;
}

int signal_send_process(pcb_t process, int sig)
{
    if (process == NULL) return -EINVAL;
    if (sig == 0) return 0;
    if (sig < MINSIG || sig > (process->linux_abi ? 64 : MAXSIG)) return -EINVAL;
    if (process->thread_queue == NULL) return -ESRCH;

    spin_lock(&process->thread_queue->lock);
    queue_foreach(process->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread == NULL || thread->status == DEATH || thread->status == OUT) continue;
        __atomic_fetch_or(&thread->signal, signal_bit(thread, sig), __ATOMIC_RELEASE);
        scheduler_wake_task(thread);
        spin_unlock(&process->thread_queue->lock);
        return 0;
    }
    spin_unlock(&process->thread_queue->lock);
    return -ESRCH;
}

int signal_send_thread(pcb_t process, int tid, int sig)
{
    if (!process || !process->thread_queue) return -ESRCH;
    if (sig < 0 || sig > (process->linux_abi ? 64 : MAXSIG)) return -EINVAL;
    spin_lock(&process->thread_queue->lock);
    for (lock_node *node = process->thread_queue->head; node; node = node->next)
    {
        tcb_t thread = (tcb_t)node->data;
        if (!thread || thread->tid != (size_t)tid || thread->status == DEATH || thread->status == OUT) continue;
        if (sig)
        {
            __atomic_fetch_or(&thread->signal, signal_bit(thread, sig), __ATOMIC_RELEASE);
            scheduler_wake_task(thread);
        }
        spin_unlock(&process->thread_queue->lock);
        return 0;
    }
    spin_unlock(&process->thread_queue->lock);
    return -ESRCH;
}

int signal_send_group(int pgid, int sig)
{
    if (pgid <= 0 || sig < MINSIG || sig > 64) return -EINVAL;
    extern lock_queue *pcb_group_queue;
    if (pcb_group_queue == NULL) return -ESRCH;

    // setpgid currently stores the group ID in pcb->pid. Match every member,
    // keeping the process list pinned until its thread queue has been visited.
    int ret = -ESRCH;
    spin_lock(&pcb_group_queue->lock);
    queue_foreach(pcb_group_queue, node)
    {
        pcb_t process = (pcb_t)node->data;
        if (process == NULL || process->pid != (size_t)pgid || process->status == DEATH ||
            process->status == OUT || process->status == ZOMBIE)
            continue;
        if (signal_send_process(process, sig) == 0) ret = 0;
    }
    spin_unlock(&pcb_group_queue->lock);
    return ret;
}

static int find_deliverable_signal(tcb_t task)
{
    if (task == NULL) return 0;
    sigset_t deliverable = __atomic_load_n(&task->signal, __ATOMIC_ACQUIRE) & ~task->blocked;
    for (int sig = MINSIG; sig <= last_signal(task); sig++) {
        if (deliverable & signal_bit(task, sig)) return sig;
    }
    return 0;
}

static bool deliver_linux_signal(X64_REGS *regs, int sig, const sigaction_t &action, int code,
                                  uint64_t address, uint64_t trap, uint64_t error, const void *saved_fpu)
{
    tcb_t task = get_current_task();
    if (!action.sa_restorer || action.sa_handler == SIG_DFL || action.sa_handler == SIG_IGN) return false;
    LinuxSignalFrame frame = {};
    frame.restorer = (uint64_t)action.sa_restorer;
    frame.context.stack = {task->sas_ss_sp, task->sas_ss_flags, task->sas_ss_size};
    frame.context.mask = task->blocked;
    frame.info.number = sig;
    frame.info.code = code;
    frame.info.address = address;
    signal_frame_t &m = frame.context.machine.arch;
    m.r8 = regs->r8; m.r9 = regs->r9; m.r10 = regs->r10; m.r11 = regs->r11;
    m.r12 = regs->r12; m.r13 = regs->r13; m.r14 = regs->r14; m.r15 = regs->r15;
    m.rdi = regs->rdi; m.rsi = regs->rsi; m.rbp = regs->rbp; m.rbx = regs->rbx;
    m.rdx = regs->rdx; m.rax = regs->rax; m.rcx = regs->rcx;
    m.rsp = regs->rsp; m.rip = regs->rip; m.eflags = regs->rflags;
    m.cs = regs->cs; m.ss = regs->ss;
    m.err = error; m.trapno = trap; m.cr2 = address; m.oldmask = task->blocked;
    bool alternate = (action.sa_flags & SA_ONSTACK) && !(task->sas_ss_flags & (SS_DISABLE | SS_ONSTACK));
    uint64_t sp = alternate ? (uint64_t)task->sas_ss_sp + task->sas_ss_size : regs->rsp;
    // Preserve the interrupted AMD64 red zone and give the handler the normal
    // SysV stack alignment. Keep the separately addressed FXSAVE image aligned.
    if (sp < 128 + sizeof(frame) + 512 + 80) return false;
    uint64_t base = ((sp - 128 - sizeof(frame) - 512 - 64) & ~15ULL) - 8;
    uint64_t fp = (base + sizeof(frame) + 63) & ~63ULL;
    m.fpstate = (struct fpstate *)fp;
    uint8_t fpstate[512] __attribute__((aligned(16))) = {};
    // FXSAVE leaves reserved bytes untouched; never expose stack padding.
    memcpy(fpstate, saved_fpu, 416);
    fpstate[5] = 0; // Reserved FXSAVE header byte is not written by the CPU.
    page_directory_t *directory = task->parent_group->pagedir;
    if (!signal_copy_to_user(directory, (void *)base, &frame, sizeof(frame)) ||
        !signal_copy_to_user(directory, (void *)fp, fpstate, sizeof(fpstate))) return false;
    if (alternate) task->sas_ss_flags |= SS_ONSTACK;
    task->blocked |= action.sa_mask;
    if (!(action.sa_flags & SIG_NOMASK)) task->blocked |= signal_bit(task, sig);
    task->blocked &= ~unmaskable(task);
    if (action.sa_flags & SIG_ONESHOT)
    {
        spin_lock(&task->parent_group->linux_signal_lock);
        __atomic_store_n(&task->parent_group->linux_actions[sig - 1].sa_handler, SIG_DFL, __ATOMIC_RELEASE);
        spin_unlock(&task->parent_group->linux_signal_lock);
    }
    regs->rip = (uint64_t)action.sa_handler;
    regs->rsp = base;
    regs->rdi = sig;
    regs->rsi = base + __builtin_offsetof(LinuxSignalFrame, info);
    regs->rdx = base + __builtin_offsetof(LinuxSignalFrame, context);
    return true;
}

bool signal_deliver_fault(X64_REGS *regs, int sig, int code, uint64_t address, uint64_t trap, uint64_t error)
{
    tcb_t task = get_current_task();
    if (!regs || (regs->cs & 3) != 3 || !linux_task(task) ||
        (task->blocked & signal_bit(task, sig))) return false;
    sigaction_t action = action_for(task, sig);
    return deliver_linux_signal(regs, sig, action, code, address, trap, error, entry_fpstate(regs));
}

static bool deliver_pending(X64_REGS *regs, const void *saved_fpu)
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || regs == NULL) return false;
    if ((regs->cs & 0x3) != 0x3) return false;

    int sig = find_deliverable_signal(task);
    if (sig == 0) return false;

    sigaction_t snapshot = action_for(task, sig);
    sigaction_t *action = &snapshot;
    sighandler_t handler = action->sa_handler;
    __atomic_fetch_and(&task->signal, ~signal_bit(task, sig), __ATOMIC_ACQ_REL);

    if (handler == SIG_IGN) return false;
    if (handler == SIG_DFL) {
        signal_internal_t decision = signal_internal_decisions[sig];
        if (decision == SIGNAL_INTERNAL_IGN || decision == SIGNAL_INTERNAL_CONT) return false;
        if (decision == SIGNAL_INTERNAL_STOP) {
            task->status = FUTEX;
            return false;
        }
        kill_proc(task->parent_group, 128 + sig, true);
        open_interrupt;
        while (true) __asm__ volatile("hlt");
    }

    if (linux_task(task)) return deliver_linux_signal(regs, sig, *action, 0, 0, 0, 0, saved_fpu);

    if (action->sa_restorer == NULL) {
        write_serial_fmt("signal: task %s signal %d has no restorer\n", task->name, sig);
        task->signal |= signal_bit(task, sig);
        return false;
    }

    page_directory_t *pagedir = task->parent_group->pagedir;
    user_signal_frame frame;
    frame.magic   = SIGNAL_FRAME_MAGIC;
    frame.sig     = sig;
    frame.oldmask = task->blocked;
    memcpy(&frame.regs, regs, sizeof(*regs));

    bool use_altstack = (action->sa_flags & SA_ONSTACK) &&
                        !(task->sas_ss_flags & SS_DISABLE) &&
                        !(task->sas_ss_flags & SS_ONSTACK);
    uint64_t sp = use_altstack ? ((uint64_t)task->sas_ss_sp + task->sas_ss_size) : regs->rsp;
    uint64_t frame_addr = (sp - sizeof(frame)) & ~0xFULL;
    uint64_t ret_addr   = frame_addr - sizeof(uint64_t);
    uint64_t restorer   = (uint64_t)action->sa_restorer;

    if (use_altstack)
        task->sas_ss_flags |= SS_ONSTACK;

    if (!signal_copy_to_user(pagedir, (void *)frame_addr, &frame, sizeof(frame)) ||
        !signal_copy_to_user(pagedir, (void *)ret_addr, &restorer, sizeof(restorer))) {
        task->signal |= signal_bit(task, sig);
        return false;
    }

    task->blocked |= action->sa_mask | SIGMASK(sig);
    task->blocked &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    if (action->sa_flags & SIG_ONESHOT) task->actions[sig].sa_handler = SIG_DFL;

    regs->rip = (uint64_t)handler;
    regs->rsp = ret_addr;
    regs->rdi = sig;
    regs->rsi = 0;
    regs->rdx = 0;
    return true;
}

bool signal_deliver_pending(X64_REGS *regs)
{
    return deliver_pending(regs, entry_fpstate(regs));
}

template <typename Destination, typename Source>
static void copy_signal_registers(Destination *to, const Source *from)
{
    to->r15 = from->r15;
    to->r14 = from->r14;
    to->r13 = from->r13;
    to->r12 = from->r12;
    to->r11 = from->r11;
    to->r10 = from->r10;
    to->r9 = from->r9;
    to->r8 = from->r8;
    to->rbx = from->rbx;
    to->rcx = from->rcx;
    to->rdx = from->rdx;
    to->rsi = from->rsi;
    to->rdi = from->rdi;
    to->rbp = from->rbp;
    to->ds = from->ds;
    to->es = from->es;
    to->rax = from->rax;
    to->rip = from->rip;
    to->cs = from->cs;
    to->rflags = from->rflags;
    to->rsp = from->rsp;
    to->ss = from->ss;
}

void signal_deliver_interrupt(struct registers *regs, const void *saved_fpu)
{
    if (!regs || (regs->cs & 3) != 3 || !saved_fpu || !signals_pending_quick(get_current_task())) return;
    // The timer has a different GPR layout from exception/syscall entry. Keep
    // both assembly ABIs unchanged, and use its private entry FXSAVE image;
    // memory adjacent to this temporary frame is not an FPU save area.
    X64_REGS frame = {};
    copy_signal_registers(&frame, regs);
    deliver_pending(&frame, saved_fpu);
    copy_signal_registers(regs, &frame);
}

uint64_t signal_return(struct X64_REGS *regs)
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || regs == NULL) return (uint64_t)-EFAULT;

    if (linux_task(task))
    {
        LinuxSignalFrame frame;
        page_directory_t *directory = task->parent_group->pagedir;
        if (regs->rsp < 8 || !signal_copy_from_user(directory, &frame, (void *)(regs->rsp - 8), sizeof(frame)))
            return (uint64_t)-EFAULT;
        signal_frame_t &m = frame.context.machine.arch;
        if (check_user_overflow(m.rip, 1) || check_user_overflow(m.rsp, 1) ||
            (m.cs & 3) != 3 || (m.ss & 3) != 3) return (uint64_t)-EINVAL;
        uint8_t fpstate[512] __attribute__((aligned(16)));
        if (m.fpstate)
        {
            if (!signal_copy_from_user(directory, fpstate, m.fpstate, sizeof(fpstate))) return (uint64_t)-EFAULT;
            uint32_t mask = ((uint32_t *)entry_fpstate(regs))[7];
            ((uint32_t *)fpstate)[6] &= mask ? mask : 0xffbf;
            memcpy(entry_fpstate(regs), fpstate, sizeof(fpstate));
        }
        task->blocked = frame.context.mask & ~unmaskable(task);
        // The alternate stack settings are restored by rt_sigreturn, including
        // a nested handler returning to an already active alternate stack.
        stack_t stack = frame.context.stack;
        if ((stack.ss_flags & ~(SS_DISABLE | SS_ONSTACK)) ||
            (!(stack.ss_flags & SS_DISABLE) && check_user_overflow((uint64_t)stack.ss_sp, stack.ss_size)))
            return (uint64_t)-EINVAL;
        task->sas_ss_sp = stack.ss_sp; task->sas_ss_size = stack.ss_size; task->sas_ss_flags = stack.ss_flags;
        regs->r8 = m.r8; regs->r9 = m.r9; regs->r10 = m.r10; regs->r11 = m.r11;
        regs->r12 = m.r12; regs->r13 = m.r13; regs->r14 = m.r14; regs->r15 = m.r15;
        regs->rdi = m.rdi; regs->rsi = m.rsi; regs->rbp = m.rbp; regs->rbx = m.rbx;
        regs->rdx = m.rdx; regs->rax = m.rax; regs->rcx = m.rcx;
        regs->rsp = m.rsp; regs->rip = m.rip;
        // Preserve only user-modifiable arithmetic/debug flags. Kernel CS/SS,
        // IOPL and interrupt control never come from the untrusted signal frame.
        regs->rflags = (m.eflags & 0x250dd5ULL) | 0x202;
        return regs->rax;
    }

    user_signal_frame frame;
    if (!signal_copy_from_user(task->parent_group->pagedir, &frame, (void *)regs->rsp, sizeof(frame))) {
        return (uint64_t)-EFAULT;
    }
    if (frame.magic != SIGNAL_FRAME_MAGIC) return (uint64_t)-EINVAL;

    task->blocked = frame.oldmask & ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    task->sas_ss_flags &= ~SS_ONSTACK;
    memcpy(regs, &frame.regs, sizeof(*regs));
    return regs->rax;
}

void signal_init() {
    signal_internal_decisions[SIGABRT] = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGALRM] = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGBUS]  = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGCHLD] = SIGNAL_INTERNAL_IGN;
    // signal_internal_decisions[SIGCLD] = SIGNAL_INTERNAL_IGN;
    signal_internal_decisions[SIGCONT] = SIGNAL_INTERNAL_CONT;
    // signal_internal_decisions[SIGEMT] = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGFPE]  = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGHUP]  = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGILL]  = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGINT]  = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGIO]   = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGIOT]  = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGKILL] = SIGNAL_INTERNAL_TERM;
    // signal_internal_decisions[SIGLOST] = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGPIPE]   = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGPOLL]   = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGPROF]   = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGPWR]    = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGQUIT]   = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGSEGV]   = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGSTKFLT] = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGSTOP]   = SIGNAL_INTERNAL_STOP;
    signal_internal_decisions[SIGTSTP]   = SIGNAL_INTERNAL_STOP;
    signal_internal_decisions[SIGSYS]    = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGTERM]   = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGTRAP]   = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGTTIN]   = SIGNAL_INTERNAL_STOP;
    signal_internal_decisions[SIGTTOU]   = SIGNAL_INTERNAL_STOP;
    signal_internal_decisions[SIGUNUSED] = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGURG]    = SIGNAL_INTERNAL_IGN;
    signal_internal_decisions[SIGUSR1]   = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGUSR2]   = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGVTALRM] = SIGNAL_INTERNAL_TERM;
    signal_internal_decisions[SIGXCPU]   = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGXFSZ]   = SIGNAL_INTERNAL_CORE;
    signal_internal_decisions[SIGWINCH]  = SIGNAL_INTERNAL_IGN;
}
