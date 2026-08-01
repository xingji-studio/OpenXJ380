#include <proto.hpp>

// int sys_exit(int status)
// {
//     return do_exit(status);
// }

// int sys_fork(struct X64_REGS *regs)
// {
//     return do_fork(regs, CLONE_VM, regs->rsp, 0);
// }

// int sys_vfork(struct X64_REGS *regs)
// {
//     return do_fork(regs, CLONE_FILES | CLONE_FS | CLONE_SIGNAL, regs->rsp, 0);
// }

// int sys_execve(struct X64_REGS *regs)
// {
//     return do_execve(regs, (const char *)regs->rdi, (char **)regs->rsi, (char **)regs->rdx);
// }

// uint64_t sys_wait4(unsigned long pid, long *status, int options, void *rusage)
// {
//     long retval = 0;
//     tcb_t *child = NULL;
//     struct task_struct *tsk = NULL;

//     for (tsk = &init_task_union.task; tsk->next != &init_task_union.task; tsk = tsk->next)
//     {
//         if (tsk->next->pid == pid)
//         {
//             child = tsk->next;
//             break;
//         }
//     }

//     if (child == NULL)
//         return -1;
//     if (options != 0)
//         return -1;

//     if (child->state == TASK_ZOMBIE)
//     {
//         xmemcpy(status, &child->exit_code, sizeof(int));
//         tsk->next = child->next;
//         return retval;
//     }

//     interruptible_sleep_on(&current_task->wait_childexit);

//     xmemcpy(status, &child->exit_code, sizeof(long));
//     tsk->next = child->next;
//     return retval;
// }

// #define ARCH_SET_GS 0x1001
// #define ARCH_SET_FS 0x1002
// #define ARCH_GET_FS 0x1003
// #define ARCH_GET_GS 0x1004

// uint64_t sys_arch_prctl(uint64_t cmd, uint64_t arg)
// {
//     switch (cmd)
//     {
//     case ARCH_SET_FS:
//         current_task->thread->fsbase = arg;
//         wrmsr(IA32_FS_BASE, current_task->thread->fsbase);
//         return 0;
//     case ARCH_SET_GS:
//         current_task->thread->gsbase = arg;
//         wrmsr(IA32_GS_BASE, current_task->thread->gsbase);
//         return 0;
//     case ARCH_GET_FS:
//         return current_task->thread->fsbase;
//     case ARCH_GET_GS:
//         return current_task->thread->gsbase;
//     default:
//         return (uint64_t)(-ENOSYS);
//     }
// }
