#include "dma.h"
#include "dma_plan.h"
#include "proto.hpp"

namespace {

struct DmaControllerPorts {
    uint16_t address;
    uint16_t count;
    uint16_t page;
    uint16_t mask;
    uint16_t mode;
    uint16_t reset_flip_flop;
};

constexpr DmaControllerPorts kPorts[8] = {
    {0x00, 0x01, 0x87, 0x0A, 0x0B, 0x0C},
    {0x02, 0x03, 0x83, 0x0A, 0x0B, 0x0C},
    {0x04, 0x05, 0x81, 0x0A, 0x0B, 0x0C},
    {0x06, 0x07, 0x82, 0x0A, 0x0B, 0x0C},
    {0xC0, 0xC2, 0x8F, 0xD4, 0xD6, 0xD8},
    {0xC4, 0xC6, 0x8B, 0xD4, 0xD6, 0xD8},
    {0xC8, 0xCA, 0x89, 0xD4, 0xD6, 0xD8},
    {0xCC, 0xCE, 0x8A, 0xD4, 0xD6, 0xD8},
};

static inline void write_word(uint16_t port, uint16_t value)
{
    outb(port, static_cast<uint8_t>(value));
    outb(port, static_cast<uint8_t>(value >> 8U));
}

static void program_transfer(uint8_t mode, uint8_t channel, const DmaTransferPlan &plan)
{
    const DmaControllerPorts &ports         = kPorts[channel];
    const uint8_t             local_channel = channel & 3U;

    close_interrupt;
    outb(ports.mask, static_cast<uint8_t>(4U | local_channel));
    outb(ports.reset_flip_flop, 0);
    outb(ports.mode, static_cast<uint8_t>(mode | local_channel));
    outb(ports.page, plan.page);
    write_word(ports.address, plan.offset);
    write_word(ports.count, plan.count);
    outb(ports.mask, local_channel);
    open_interrupt;
}

}

void dma_start(uint8_t mode, uint8_t channel, uint32_t *address, uint32_t size)
{
    DmaTransferPlan plan = {};
    const auto      physical_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address));
    if (dma_plan_transfer(channel, physical_address, size, &plan)) {
        program_transfer(mode, channel, plan);
    }
}

void dma_send(uint8_t channel, uint32_t *address, uint32_t size)
{
    dma_start(0x48, channel, address, size);
}

void dma_recv(uint8_t channel, uint32_t *address, uint32_t size)
{
    dma_start(0x44, channel, address, size);
}
