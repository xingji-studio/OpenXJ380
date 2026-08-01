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

signal_internal_t signal_internal_decisions[MAXSIG + 1];

static bool signal_copy_to_user(page_directory_t *pagedir, void *dst, const void *src, size_t size)
{
    if (size == 0) return true;
    if (src == NULL) return false;
    if (!user_range_mapped(pagedir, dst, size)) return false;
    memcpy(dst, src, size);
    return true;
}

static bool signal_copy_from_user(page_directory_t *pagedir, void *dst, const void *src, size_t size)
{
    if (size == 0) return true;
    if (dst == NULL) return false;
    if (!user_range_mapped(pagedir, src, size)) return false;
    memcpy(dst, src, size);
    return true;
}

bool signals_pending_quick(tcb_t task) {
    if (task == NULL) return false;
    sigset_t pending_list   = task->signal;
    sigset_t unblocked_list = pending_list & (~task->blocked);
    for (int i = MINSIG; i <= MAXSIG; i++) {
        if (!(unblocked_list & SIGMASK(i))) continue;
        sigaction_t *action       = &task->actions[i];
        sighandler_t user_handler = action->sa_handler;
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
        safe         &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
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
    if (sig < MINSIG || sig > MAXSIG || sig == SIGKILL || sig == SIGSTOP) { return -EINVAL; }

    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return -ESRCH;

    page_directory_t *pagedir = task->parent_group->pagedir;
    sigaction_t *ptr = &task->actions[sig];
    if (oldaction && !signal_copy_to_user(pagedir, oldaction, ptr, sizeof(*ptr))) return -EFAULT;

    if (action && !signal_copy_from_user(pagedir, ptr, action, sizeof(*ptr))) return -EFAULT;

    if (ptr->sa_flags & SIG_NOMASK) {
        ptr->sa_mask = 0;
    } else {
        ptr->sa_mask |= SIGMASK(sig);
    }

    return 0;
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
    if (sig < MINSIG || sig > MAXSIG) return -EINVAL;
    if (process->thread_queue == NULL) return -ESRCH;

    spin_lock(&process->thread_queue->lock);
    queue_foreach(process->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread == NULL || thread->status == DEATH || thread->status == OUT) continue;
        thread->signal |= SIGMASK(sig);
        spin_unlock(&process->thread_queue->lock);
        return 0;
    }
    spin_unlock(&process->thread_queue->lock);
    return -ESRCH;
}

static int find_deliverable_signal(tcb_t task)
{
    if (task == NULL) return 0;
    sigset_t deliverable = task->signal & ~task->blocked;
    for (int sig = MINSIG; sig <= MAXSIG; sig++) {
        if (deliverable & SIGMASK(sig)) return sig;
    }
    return 0;
}

bool signal_deliver_pending(struct X64_REGS *regs)
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || regs == NULL) return false;
    if ((regs->cs & 0x3) != 0x3) return false;

    int sig = find_deliverable_signal(task);
    if (sig == 0) return false;

    sigaction_t *action = &task->actions[sig];
    sighandler_t handler = action->sa_handler;
    task->signal &= ~SIGMASK(sig);

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

    if (action->sa_restorer == NULL) {
        write_serial_fmt("signal: task %s signal %d has no restorer\n", task->name, sig);
        task->signal |= SIGMASK(sig);
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
        task->signal |= SIGMASK(sig);
        return false;
    }

    task->blocked |= action->sa_mask | SIGMASK(sig);
    task->blocked &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    if (action->sa_flags & SIG_ONESHOT) action->sa_handler = SIG_DFL;

    regs->rip = (uint64_t)handler;
    regs->rsp = ret_addr;
    regs->rdi = sig;
    regs->rsi = 0;
    regs->rdx = 0;
    return true;
}

uint64_t signal_return(struct X64_REGS *regs)
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || regs == NULL) return (uint64_t)-EFAULT;

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
