#include <cpu/longm.h>
#include <proto.hpp>
#include <stdint.h>
#include <task/scheduler.h>
#include <cpu/regio.h>
#include <wsod.h>

extern bool no_interrupt;

static bool deliver_user_exception(X64_REGS *regs, int sig, int code, uint64_t trap, uint64_t address)
{
    tcb_t task = get_current_task();
    if (!regs || (regs->cs & 3) != 3 || !task || !task->parent_group ||
        !task->parent_group->linux_abi || scheduler_disable_depth()) return false;
    if (!no_interrupt) open_interrupt;
    bool delivered = signal_deliver_fault(regs, sig, code, address, trap, regs->errcode);
    close_interrupt;
    if (delivered) return true;
    // A Linux application's unhandled synchronous exception terminates that
    // process; it must never close the global boot scheduler gate.
    kill_proc(task->parent_group, 128 + sig, true);
    open_interrupt;
    scheduler_yield();
    while (true) __asm__ volatile("hlt");
}

void process_wsod(const char *err_code, struct X64_REGS *regs) __attribute__((weak));
void process_wsod_without_regs(const char *err_code) __attribute__((weak));

[[noreturn]] void do_wsod(const char *err_code, struct X64_REGS *regs)
{
    if (process_wsod) process_wsod(err_code, regs);
    if (wsod_begin(regs))
    {
        wsod_stop_other_cpus();
        write_serial_fmt("OpenXJ380 fatal: %s RIP=0x%llx\n", err_code, regs ? regs->rip : 0);
    }
    wsod_halt();
}

[[noreturn]] void no_regs_wsod(const char *err_code)
{
    if (process_wsod_without_regs) process_wsod_without_regs(err_code);
    if (wsod_begin(nullptr))
    {
        wsod_stop_other_cpus();
        write_serial_fmt("OpenXJ380 fatal: %s (no exception frame)\n", err_code);
    }
    wsod_halt();
}

/**
 *
 *      异常处理函数
 *
 */

// 预期外的中断
void wsod_unexpect_intrrupt(struct X64_REGS *regs, uint64_t error_code)
{
    do_wsod("UNEXPECT_INTRRUPT", regs);
}

void c_device_not_avaliable(struct X64_REGS *regs, uint64_t error_code)
{
    __asm__ __volatile__("movq %%cr0, %%rax\n\t"
                         "and $0xFFF3, %%ax	\n\t" // clear coprocessor emulation CR0.EM and CR0.TS
                         "or $0x2, %%ax\n\t"      // set coprocessor monitoring  CR0.MP
                         "movq %%rax, %%cr0\n\t" ::
                             : "rax");
}

void wsod_divide_error(struct X64_REGS *regs, uint64_t error_code)
{
    if (deliver_user_exception(regs, SIGFPE, 1, 0, regs->rip)) return;
    do_wsod("DIVIDE_ERROR", regs);
}

void wsod_undefined_opcode(struct X64_REGS *regs, uint64_t error_code)
{
    if (deliver_user_exception(regs, SIGILL, 2, 6, regs->rip)) return;
    do_wsod("UNDEFINED_OPCODE", regs);
}

// 不可屏蔽中断
void wsod_nmi(struct X64_REGS *regs, uint64_t error_code)
{
    if (wsod_nmi_stop(regs)) return;
    do_wsod("NMI", regs);
}

// 双重错误
void wsod_double_fault(struct X64_REGS *regs, uint64_t error_code)
{
    do_wsod("DOUBLE_FAULT", regs);
}

// 内核错误
void wsod_system_kernel_error(struct X64_REGS *regs, uint64_t error_code)
{
    do_wsod("SYSTEM_KERNEL_ERROR", regs);
}

// 一般保护性异常
void wsod_general_protection(struct X64_REGS *regs, uint64_t error_code)
{
    if (deliver_user_exception(regs, SIGSEGV, 128, 13, 0)) return;
    do_wsod("GENERAL_PROTECTION", regs);
}

// DEBUG
void wsod_debug(struct X64_REGS *regs, uint64_t error_code)
{
    if (deliver_user_exception(regs, SIGTRAP, 1, 3, regs->rip)) return;
    do_wsod("DEBUG_BREAKPOINT", regs);
}

void default_isr(struct X64_REGS *regs, uint64_t error_code)
{
    // 啥也没有。(*最好是)
    send_eoi();
}
