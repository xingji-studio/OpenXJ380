#include <cpu/longm.h>
#include <proto.hpp>
#include <stdint.h>

int lookup_kallsyms(uint64_t address)
{
    write_serial_string("backtrace address:");
    write_serial_hex(address);
    write_serial_string("\n");
    return 0;
}

void backtrace(struct X64_REGS *regs)
{
    uint64_t *rbp = (uint64_t *)regs->rbp;
    if (regs->rbp < 0xFFFF800000000000)
    {
        write_serial_string("Fault in user land\n");
        return;
    }
    uint64_t ret_address = *(rbp + 1);
    int i = 0;

    write_serial_string("Kernel backtrace:\n");

    lookup_kallsyms(regs->rip);
    for (i = 0; i < 8; i++)
    {
        if (lookup_kallsyms(ret_address))
            break;

        if ((uint64_t)rbp >= get_current_task()->context0.rsp || (uint64_t)rbp < regs->rsp)
            break;

        if (rbp == nullptr)
        {
            break;
        }
        rbp = (uint64_t *)*rbp;
        if (rbp == nullptr)
        {
            break;
        }
        ret_address = *(rbp + 1);
    }

    write_serial_string("Kernel backtrace end\n");
}