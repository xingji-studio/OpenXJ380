#include <apic/hpet.h>
#include <dlinker.h>
#include <proto.hpp>
#include <stdint.h>

HpetInfo       *info;
static uint32_t hpetPeriod = 0;
static uint64_t hpetBootNano = 0;

uint64_t nanoTime()
{
    if (info == (void *)0) return 0;
    uint64_t mcv = info->mainCounterValue;
    return mcv * hpetPeriod;
}
EXPORT_SYMBOL(nanoTime);

uint64_t bootNanoTime()
{
    uint64_t now = nanoTime();
    if (hpetBootNano == 0 || now < hpetBootNano) return 0;
    return now - hpetBootNano;
}
EXPORT_SYMBOL(bootNanoTime);

void nsleep(uint64_t nano)
{
    uint64_t targetTime = nanoTime();
    uint64_t after      = 0;
    while (1)
    {
        uint64_t n = nanoTime();
        if (n < targetTime)
        {
            after      += 0xffffffff - targetTime + n;
            targetTime  = n;
        }
        else
        {
            after      += n - targetTime;
            targetTime  = n;
        }
        if (after >= nano) { return; }
    }
}

void init_hpet(uint64_t hpet_ptr)
{
    info                         = (HpetInfo *)(((Hpet *)hpet_ptr)->base_address.address + 0xFFFF800000000000);
    uint32_t counterClockPeriod  = info->generalCapabilities >> 32;
    hpetPeriod                   = counterClockPeriod / 1000000;
    info->generalConfiguration  |= 1;
    hpetBootNano                 = nanoTime();
    write_serial_string("Setup acpi hpet table (nano_time: ");
    write_serial_dec((uint64_t)nanoTime());
    write_serial_string(").\n");
}
