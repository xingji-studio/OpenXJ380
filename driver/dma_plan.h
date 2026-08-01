#pragma once

#include <stdint.h>

struct DmaTransferPlan {
    uint8_t  page;
    uint16_t offset;
    uint16_t count;
};

static inline bool dma_plan_transfer(uint8_t channel, uint32_t address, uint32_t byte_count,
                                     DmaTransferPlan *plan)
{
    if (plan == nullptr || channel > 7 || channel == 4 || byte_count == 0 || address >= 0x01000000U ||
        byte_count > 0x01000000U - address) {
        return false;
    }

    const uint32_t last_address = address + byte_count - 1U;
    const bool     word_channel = channel >= 5;
    if (word_channel) {
        if ((address & 1U) != 0 || (byte_count & 1U) != 0 || (address >> 17U) != (last_address >> 17U)) {
            return false;
        }
    } else if ((address >> 16U) != (last_address >> 16U)) {
        return false;
    }

    const uint32_t units = word_channel ? byte_count / 2U : byte_count;
    if (units == 0 || units > 0x10000U) {
        return false;
    }

    plan->page   = static_cast<uint8_t>(address >> 16U);
    plan->offset = static_cast<uint16_t>(word_channel ? address >> 1U : address);
    plan->count  = static_cast<uint16_t>(units - 1U);
    return true;
}
