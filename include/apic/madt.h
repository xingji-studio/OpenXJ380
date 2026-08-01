#ifndef _MADT_H_
#define _MADT_H_

#pragma pack(1)
#ifdef __cplusplus
extern "C" {
#endif
#include <efi/efi.h>
#ifdef __cplusplus
}
#endif

typedef struct
{
    uint32_t LocalApicAddr; // Local APIC 地址
    uint32_t PicSign;       // 1=安装了8259A
} __attribute__((packed)) MADT_FLAG;

typedef struct
{
    uint8_t EntryType;
    uint8_t length;
} __attribute__((packed)) MADT_ENTRY_FLAG;

// EntryType = 0
typedef struct
{
    uint8_t  AcpiProcessorID;
    uint8_t  ApicID;
    uint32_t Flags; // bit 0 = Processor Enabled, bit 1 = Online Capable 我也不知道啥意思
} __attribute__((packed)) MADT_LOCAL_APIC_FLAG;

// EntryType = 1
typedef struct
{
    uint8_t  IOApicID;
    uint8_t  reserved;
    uint32_t IOApicAddr;
    uint32_t GlobalSysIntrBase;
} __attribute__((packed)) MADT_IO_APIC_FLAG;

// EntryType = 2
typedef struct
{
    uint8_t  BusScr;
    uint8_t  IrqScr;
    uint32_t GlobalSysIntr;
    uint16_t Flags;
} __attribute__((packed)) MADT_IO_APIC_InterScrOverride;

// EntryType = 3
typedef struct
{
    uint8_t  NMIScr;
    uint8_t  reserved;
    uint16_t Flags;
    uint32_t GlobalSysIntr;
} __attribute__((packed)) MADT_IO_APIC_NMIScr;

// EntryType = 4
typedef struct
{
    uint8_t  AcpiProcessorID;
    uint16_t Flags;
    uint8_t  LINT; // LINT#（0或1）
} __attribute__((packed)) MADT_LOCAL_APIC_NMI;

// EntryType = 5
typedef struct
{
    uint16_t reserved;
    uint64_t LocalApicAddr;
} __attribute__((packed)) MADT_LOCAL_APIC_AddrOverride;

// EntryType = 9
typedef struct
{
    uint16_t reserved;
    uint32_t LocalApicID;
    uint32_t Flags; // bit 0 = Processor Enabled, bit 1 = Online Capable 我也不知道啥意思
    uint32_t AcpiFlags;
} __attribute__((packed)) MADT_LOCAL_x2APIC;

#pragma pack()

#endif