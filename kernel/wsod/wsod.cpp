#include <cpu/longm.h>
#include <proto.hpp>
#include <stdint.h>
#include <task/scheduler.h>
#include <cpu/regio.h>

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


void do_wsod(const char *err_code, struct X64_REGS *regs)
{
    // TODO: WSOD

    disable_intr();
    scheduler_stop();
    while (1)
    {
        __asm__("pause");
    };
}

void no_regs_wsod(const char *err_code)
{
    // TODO: WSOD

    disable_intr();
    scheduler_stop();
    while (1)
    {
        __asm__ __volatile__("pause");
    };
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
    scheduler_stop();

    write_serial_string("Undefined opcode at RIP: ");
    write_serial_hex(regs->rip);
    write_serial_string(" RSP: ");
    write_serial_hex(regs->rsp);
    write_serial_string(" CS: ");
    write_serial_hex(regs->cs);
    write_serial_string("\n");

    write_serial_string("Fault Thread Name: ");
    if (get_current_task() != NULL)
    {
        write_serial_string(get_current_task()->name);
        write_serial_string(" ");
        write_serial_hex((uint64_t)get_current_task());
    }
    else
    {
        write_serial_string("<none>");
    }
    write_serial_string("\n");

    if (regs->rip >= 8 && (regs->cs & 0x3) == 0x3)
    {
        write_serial_string("Opcode bytes: ");
        uint8_t *code = (uint8_t *)(regs->rip - 8);
        for (int i = 0; i < 24; i++)
        {
            write_serial_hex(code[i]);
            write_serial_string(" ");
        }
        write_serial_string("\n");
    }

    do_wsod("UNDEFINED_OPCODE", regs);
}

// 不可屏蔽中断
void wsod_nmi(struct X64_REGS *regs, uint64_t error_code)
{
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
    scheduler_stop();

    write_serial_string("Fault Thread Name: ");
    write_serial_string(get_current_task()->name);
    write_serial_string(" ");
    write_serial_hex((uint64_t)get_current_task());
    write_serial_string("\n");

    if (error_code & 0x01)
        write_serial_string(
            "The exception occurred during delivery of an event external to the program,such as an interrupt "
            "or an earlier exception.\n");

    if (error_code & 0x02)
        write_serial_string("Refers to a gate descriptor in the IDT;\n");
    else
        write_serial_string("Refers to a descriptor in the GDT or the current LDT;\n");

    if ((error_code & 0x02) == 0)
    {
        if (error_code & 0x04)
            write_serial_string("Refers to a segment or gate descriptor in the LDT;\n");
        else
            write_serial_string("Refers to a descriptor in the current GDT;\n");
    }

    write_serial_string("Error Code:");
    write_serial_hex(error_code);
    write_serial_string("\n");

    write_serial_string("Adrress:");
    write_serial_hex(regs->rip);
    write_serial_string("\n");

    backtrace(regs);

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
