#ifndef _APIC_H_
#define _APIC_H_

#pragma pack(push, 1)
#ifdef __cplusplus
extern "C" {
#endif
#include <efi/efi.h>
#ifdef __cplusplus
}
#endif

#include <stdint.h>

// CPUID命令
#define CPUID_ECX_x2APIC 1 << 21
#define CPUID_EDX_APIC   1 << 9

// MSR IA32_APIC_BASE
#define MSR_IA32_APIC_BASE      0x1b
#define xAPIC_ENABLE            1 << 11
#define x2APIC_ENABLE           1 << 10
#define IA32_APIC_SVR_MSR_LAPIC 1 << 8

// LocalAPIC寄存器偏移量
// 0x00 ~ 0x10 Reserved.
#define LAPIC_ID_OFFEST       0x20
#define LAPIC_VERSION_OFFEST  0x30
// 0x40 ~ 0x70 Reserved.
#define LAPIC_TPR_OFFEST      0x80
#define LAPIC_APR_OFFEST      0x90
#define LAPIC_PPR_OFFEST      0xa0
#define LAPIC_EOI_OFFEST      0xb0
#define LAPIC_RRD_OFFEST      0xc0
#define LAPIC_LDR_OFFEST      0xd0
#define LAPIC_DFR_OFFEST      0xe0
#define LAPIC_SVR_OFFEST      0xf0
// 0x100-0x2e0 are reserved until a concrete interrupt vector is assigned.
#define LAPIC_LVT_CMCI_OFFEST 0x2f0
#define LAPIC_ICR_OFFEST      0x300
#define LAPIC_REG_ICR0        0x300
#define LAPIC_REG_ICR1        0x310
#define LAPIC_REG_EOI         0x0b0

#define LAPIC_LVT_TIMER_OFFEST   0x320
#define LAPIC_LVT_THERMAL_OFFEST 0x330
#define LAPIC_LVT_PERFORM_OFFEST 0x340
#define LAPIC_LVT_LINT0_OFFEST   0x350
#define LAPIC_LVT_LINT1_OFFEST   0x360
#define LAPIC_LVT_ERROR_OFFEST   0x370
#define LAPIC_INITCNT_OFFEST     0x380
#define LAPIC_CURCNT_OFFEST      0x390
// 0x3a0 ~ 0x3d0 Reserved.
#define LAPIC_DIVTIMER_OFFEST    0x3e0

struct APIC_INFO
{
    bool     x2Apic;
    uint64_t LocalApicAddr;
    uint64_t ioApicAddr;
};

#define MADT_APIC_LOCAL   0x00
#define MADT_APIC_IO      0x01
#define MADT_X2APIC_LOCAL 0x09

#define LAPIC_REG_ID            0x020
#define LAPIC_REG_TIMER_CURCNT  0x390
#define LAPIC_REG_TIMER_INITCNT 0x380
#define LAPIC_REG_TIMER         0x320
#define LAPIC_REG_SPURIOUS      0xf0
#define LAPIC_REG_TIMER_DIV     0x3e0

#include <acpi/acpi.h>

typedef struct
{
    struct ACPISDTHeader h;
    uint32_t             local_apic_address;
    uint32_t             flags;
    void                *entries;
} __attribute__((packed)) MADT;

struct madt_hander
{
    uint8_t entry_type;
    uint8_t length;
} __attribute__((packed));

struct madt_local_apic
{
    // type=0
    struct madt_hander h;
    uint8_t            ACPI_Processor_UID;
    // 处理器的local apic id
    uint8_t            local_apic_id;
    // 详见 ACPI Specification Version 6.3, Table 5-47
    uint32_t           flags;
};

struct madt_x2_localapic
{
    struct madt_hander h;
    uint8_t            reserved[2];
    uint32_t           x2apic_id;
    uint32_t           flags;
    uint32_t           acpi_processor_uid;
} __attribute__((packed));

struct madt_io_apic
{
    struct madt_hander h;
    uint8_t            apic_id;
    uint8_t            reserved;
    uint32_t           address;
    uint32_t           gsib;
} __attribute__((packed));

typedef struct madt_hander       MadtHeader;
typedef struct madt_local_apic   MadtLocalApic;
typedef struct madt_x2_localapic MadtLocalX2apic;
typedef struct madt_io_apic      MadtIOApic;

#pragma pack(pop)

#endif
