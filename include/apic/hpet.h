#ifndef __HPET_H__
#define __HPET_H__

#pragma pack(1)
#include <stdint.h>

struct generic_address
{
    uint8_t  address_space;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

struct ACPISDTHeader
{
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OEMID[6];
    char     OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
};

struct hpet
{
    struct ACPISDTHeader   h;
    uint32_t               event_block_id;
    struct generic_address base_address;
    uint16_t               clock_tick_unit;
    uint8_t                page_oem_flags;
} __attribute__((packed));

typedef struct
{
    uint64_t configurationAndCapability;
    uint64_t comparatorValue;
    uint64_t fsbInterruptRoute;
    uint64_t unused;
} __attribute__((packed)) HpetTimer;

typedef struct
{
    uint64_t  generalCapabilities;
    uint64_t  reserved0;
    uint64_t  generalConfiguration;
    uint64_t  reserved1;
    uint64_t  generalIntrruptStatus;
    uint8_t   reserved3[0xc8];
    uint64_t  mainCounterValue;
    uint64_t  reserved4;
    HpetTimer timers[];
} __attribute__((packed)) volatile HpetInfo;

typedef struct generic_address GenericAddress;
typedef struct hpet            Hpet;

#pragma pack()

#endif