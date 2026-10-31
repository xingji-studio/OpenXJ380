// SPDX-License-Identifier: Apache-2.0
#include <apic/hpet.h>
#include <dlinker.h>
#include <proto.hpp>
#include <stdint.h>

HpetInfo       *info;
static uint32_t hpetPeriodFs = 0;
static uint64_t hpetBootNano = 0;

static uint64_t hpet_read_counter()
{
    volatile uint32_t *counter = (volatile uint32_t *)((uintptr_t)info + __builtin_offsetof(HpetInfo, mainCounterValue));
    uint32_t high_before, low, high_after;
    // A CPU's 64-bit load may reach an HPET as separate 32-bit MMIO accesses.
    // If low wraps between those accesses, low-then-high combines different
    // counter epochs and jumps forward by 2^32 ticks. Retry on a changed high
    // word instead. All state is local, including when CPUs read concurrently.
    do
    {
        high_before = counter[1];
        low         = counter[0];
        high_after  = counter[1];
    } while (high_before != high_after);
    return ((uint64_t)high_before << 32) | low;
}

uint64_t nanoTime()
{
    if (info == (void *)0) return 0;
    uint64_t mcv = hpet_read_counter();
    // The capability period is in femtoseconds, which need not be an integer
    // number of nanoseconds. Divide before multiplying the large tick count;
    // the remainder product fits in 64 bits even for a full 32-bit period.
    return (mcv / 1000000ULL) * hpetPeriodFs + (mcv % 1000000ULL) * hpetPeriodFs / 1000000ULL;
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
    hpetPeriodFs                 = counterClockPeriod;
    info->generalConfiguration  |= 1;
    hpetBootNano                 = nanoTime();
    write_serial_string("Setup acpi hpet table (nano_time: ");
    write_serial_dec((uint64_t)nanoTime());
    write_serial_string(").\n");
}
