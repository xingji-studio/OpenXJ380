#pragma once

#include <acpi/acpi.h>

#pragma pack(push, 1)

typedef struct GenericAddressStructure
{
    uint8_t  address_space_id;
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed)) GenericAddressStructure;

typedef struct FixedAcpiDescriptionTable
{
    ACPI_TABLE_HEADER      h;
    uint32_t               firmware_ctrl;
    uint32_t               dsdt;
    uint8_t                reserved0;
    uint8_t                preferred_pm_profile;
    uint16_t               sci_int;
    uint32_t               smi_cmd;
    uint8_t                acpi_enable;
    uint8_t                acpi_disable;
    uint8_t                s4bios_req;
    uint8_t                pstate_cnt;
    uint32_t               pm1a_evt_blk;
    uint32_t               pm1b_evt_blk;
    uint32_t               pm1a_cnt_blk;
    uint32_t               pm1b_cnt_blk;
    uint32_t               pm2_cnt_blk;
    uint32_t               pm_tmr_blk;
    uint32_t               gpe0_blk;
    uint32_t               gpe1_blk;
    uint8_t                pm1_evt_len;
    uint8_t                pm1_cnt_len;
    uint8_t                pm2_cnt_len;
    uint8_t                pm_tmr_len;
    uint8_t                gpe0_blk_len;
    uint8_t                gpe1_blk_len;
    uint8_t                gpe1_base;
    uint8_t                cst_cnt;
    uint16_t               p_lvl2_lat;
    uint16_t               p_lvl3_lat;
    uint16_t               flush_size;
    uint16_t               flush_stride;
    uint8_t                duty_offset;
    uint8_t                duty_width;
    uint8_t                day_alrm;
    uint8_t                mon_alrm;
    uint8_t                century;
    uint16_t               iapc_boot_arch;
    uint8_t                reserved1;
    uint32_t               flags;
    GenericAddressStructure reset_reg;
    uint8_t                reset_value;
    uint8_t                reserved2[3];
    uint64_t               x_firmware_ctrl;
    uint64_t               x_dsdt;
} __attribute__((packed)) FixedAcpiDescriptionTable;

#pragma pack(pop)

#define ACPI_SLP_EN (1u << 13)
