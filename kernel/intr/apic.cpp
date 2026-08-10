#include <apic/apic.h>
#include <apic/madt.h>
#include <cpu/msr.h>
#include <proto.hpp>

APIC_INFO ApicInfo;

/**
 *
 * @brief 检测APIC是否存在
 *
 * @return 0=NotSupport 1=APIC 2=x2APIC
 *
 */
uint8_t check_apic()
{
    uint32_t cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx;
    asm_cpuid(1, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    if (cpuid_ecx & CPUID_ECX_x2APIC)
    {
        // 支持x2APIC
        return 2;
    }
    else if (cpuid_edx & CPUID_EDX_APIC)
    {
        // 支持xAPIC
        return 1;
    }
    return 0;
}

static inline void mmio_write32(uint64_t addr, uint32_t data)
{
    asm volatile("movl %1, (%0)" : : "r"(addr), "ir"(data) : "memory");
}

static inline uint32_t mmio_read32(uint64_t addr)
{
    uint32_t ret;
    asm volatile("movl (%1), %0" : "=r"(ret) : "r"(addr) : "memory");
    return ret;
}

void lapic_write(uint32_t reg, uint64_t value)
{
    if (ApicInfo.x2Apic)
    {
        wrmsr(0x800 + (reg >> 4), value);
        return;
    }
    mmio_write32((uint64_t)ApicInfo.LocalApicAddr + reg, (uint32_t)value);
}

uint32_t lapic_read(uint32_t reg)
{
    if (ApicInfo.x2Apic) { return rdmsr(0x800 + (reg >> 4)); }
    return mmio_read32((uint64_t)ApicInfo.LocalApicAddr + reg);
}

uint64_t lapic_id()
{
    if (ApicInfo.x2Apic) { return lapic_read(LAPIC_REG_ID); }
    return (lapic_read(LAPIC_REG_ID) >> 24);
}

bool lapic_send_fixed_ipi(uint32_t destination_lapic_id, uint8_t vector)
{
    if (ApicInfo.x2Apic)
    {
        lapic_write(LAPIC_REG_ICR0, ((uint64_t)destination_lapic_id << 32) | vector);
        return true;
    }

    lapic_write(LAPIC_REG_ICR1, (uint64_t)destination_lapic_id << 24);
    lapic_write(LAPIC_REG_ICR0, vector);
    return true;
}

static void ioapic_write(uint32_t reg, uint32_t value)
{
    mmio_write32(ApicInfo.ioApicAddr, reg);
    mmio_write32(ApicInfo.ioApicAddr + 0x10, value);
}

static uint32_t ioapic_read(uint32_t reg)
{
    mmio_write32(ApicInfo.ioApicAddr, reg);
    return mmio_read32(ApicInfo.ioApicAddr + 0x10);
}

void ioapic_disable(uint8_t vector)
{
    uint64_t index  = 0x10 + ((vector - 32) * 2);
    uint64_t value  = (uint64_t)ioapic_read(index + 1) << 32 | (uint64_t)ioapic_read(index);
    value          |= (0x10000UL);
    ioapic_write(index, (uint32_t)(value & 0xFFFFFFFF));
    ioapic_write(index + 1, (uint32_t)(value >> 32));
}

uint32_t ioapic_gsi_count()
{
    return ((ioapic_read(1) & 0xff0000) >> 16) + 1;
}

void ioapic_mask_all()
{
    uint32_t gsi_count = ioapic_gsi_count();
    for (uint32_t j = 0; j < gsi_count; j++)
    {
        uint64_t ioredtbl = j * 2 + 16;
        switch ((ioapic_read(ioredtbl) >> 8) & 0b111)
        {
        case 0b000: // Fixed
        case 0b001: // Lowest Priority
            break;
        default: continue;
        }

        ioapic_write(ioredtbl, (1 << 16)); // mask
        ioapic_write(ioredtbl + 1, 0);
    }
}

void ioapic_add(uint8_t vector, uint32_t irq)
{
    uint32_t ioredtbl  = (uint32_t)(0x10 + (uint32_t)(irq * 2));
    uint64_t redirect  = (uint64_t)vector;
    redirect          |= lapic_id() << 56;
    ioapic_write(ioredtbl, (uint32_t)redirect);
    ioapic_write(ioredtbl + 1, (uint32_t)(redirect >> 32));
}

void init_IOApic()
{
    write_serial_string("Initializing IOAPIC...\n");

    ioapic_add((uint8_t)32, 0);
    ioapic_add((uint8_t)33, 1);
    ioapic_add((uint8_t)34, 12);
    ioapic_add((uint8_t)40, 5);

    write_serial_string("Setup I/O apic.\n");

    write_serial_string("IOAPIC Address: ");
    write_serial_hex(ApicInfo.ioApicAddr);
    write_serial_string("\n");
    write_serial_string("IOAPIC Initialize Success.\n");
}

/**
 *
 * @brief 初始化(Local) x2APIC
 *
 * @param MADT MADT表地址
 *
 */
void init_lApic()
{
    lapic_write(LAPIC_REG_SPURIOUS, 0xff | 1 << 8);
    lapic_write(LAPIC_REG_TIMER, 32); // 32为 ioapic 时钟中断向量
    lapic_write(LAPIC_REG_TIMER_DIV, 11);
    lapic_write(LAPIC_REG_TIMER_INITCNT, ~((uint32_t)0));
    uint64_t b = nanoTime();
    uint64_t guard = 0;
    for (;;)
    {
        if (nanoTime() - b >= 1000000) break;
        if (++guard >= 50000000ULL)
        {
            write_serial_string("APIC timer calibration timeout, using fallback value.\n");
            lapic_write(LAPIC_REG_TIMER, lapic_read(LAPIC_REG_TIMER) | 1 << 17);
            lapic_write(LAPIC_REG_TIMER_INITCNT, 0x100000);
            return;
        }
    }
    uint64_t lapic_timer              = (~(uint32_t)0) - lapic_read(LAPIC_REG_TIMER_CURCNT);
    uint64_t calibrated_timer_initial = (uint64_t)((uint64_t)(lapic_timer * 1000) / 250);
    if (calibrated_timer_initial == 0)
    {
        write_serial_string("APIC timer calibration produced zero, using fallback value.\n");
        calibrated_timer_initial = 0x100000;
    }
    lapic_write(LAPIC_REG_TIMER, lapic_read(LAPIC_REG_TIMER) | 1 << 17);
    lapic_write(LAPIC_REG_TIMER_INITCNT, calibrated_timer_initial);
}

/**
 *
 * @brief 初始化Local APIC
 *
 * @param MADT MADT表地址
 *
 */
void init_apic(uint64_t MADT0)
{
    write_serial_string("Initializing APIC...\n");
    __asm__ volatile("cli" ::: "memory");

    // 屏蔽8259A
    disable_8259A();
    write_serial_string("Mask 8259A Success.\n");
    io_mfence();

    switch (check_apic())
    {
    case 2:
        write_serial_string("Your PC support x2APIC.\n");
        ApicInfo.x2Apic = true;
        break;
    case 1:
        write_serial_string("Your PC support xAPIC.\n");
        ApicInfo.x2Apic = false;
        break;
    default:
        while (1)
            ;
        break; // 虽然这里理论上永远都不会执行...
    }

    uint64_t ia32_apic_base  = rdmsr(0x1b);
    ia32_apic_base          |= 1 << 11;
    if (ApicInfo.x2Apic) { ia32_apic_base |= 1 << 10; }
    wrmsr(0x1b, ia32_apic_base);

    MADT *madt             = (MADT *)MADT0;
    ApicInfo.LocalApicAddr = (uint64_t)madt->local_apic_address + 0xffff800000000000;
    uint64_t current       = 0;
    for (;;)
    {
        if (current + ((uint32_t)sizeof(MADT) - 1) >= madt->h.Length) { break; }
        MadtHeader *header = (MadtHeader *)((uint64_t)(&madt->entries) + current);
        if (header->entry_type == MADT_APIC_IO)
        {
            MadtIOApic *ioapic  = (MadtIOApic *)((uint64_t)(&madt->entries) + current);
            ApicInfo.ioApicAddr = (uint64_t)ioapic->address + 0xffff800000000000;
            break;
        }
        current += (uint64_t)header->length;
    }

    init_lApic();
    init_IOApic();


    write_serial_string("APIC Initialize Success.\n");
}

void send_eoi()
{
    lapic_write(0xb0, 0);
}
