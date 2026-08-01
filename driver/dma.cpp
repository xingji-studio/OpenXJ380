#include "dma.h"
#include "proto.hpp"

namespace {

constexpr uint8_t kDmaMaskReg[8] = {0x0A, 0x0A, 0x0A, 0x0A, 0xD4, 0xD4, 0xD4, 0xD4};
constexpr uint8_t kDmaModeReg[8] = {0x0B, 0x0B, 0x0B, 0x0B, 0xD6, 0xD6, 0xD6, 0xD6};
constexpr uint8_t kDmaClearReg[8] = {0x0C, 0x0C, 0x0C, 0x0C, 0xD8, 0xD8, 0xD8, 0xD8};
constexpr uint8_t kDmaPagePort[8] = {0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A};
constexpr uint8_t kDmaAddrPort[8] = {0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC};
constexpr uint8_t kDmaCountPort[8] = {0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE};

constexpr uint32_t kIsaDmaAddressLimit = 1u << 24;

static inline bool dma_channel_is_16bit(uint8_t channel)
{
    return channel >= 5 && channel <= 7;
}

static inline bool dma_channel_is_valid(uint8_t channel)
{
    return channel <= 7 && channel != 4;
}

static inline uint16_t dma_low16(uint32_t value)
{
    return static_cast<uint16_t>(value & 0xFFFFu);
}

static inline uint8_t dma_low8(uint16_t value)
{
    return static_cast<uint8_t>(value & 0xFFu);
}

static inline uint8_t dma_high8(uint16_t value)
{
    return static_cast<uint8_t>((value >> 8) & 0xFFu);
}

static bool dma_prepare_transfer(uint8_t channel, uint32_t address, uint32_t size,
                                 uint8_t *page, uint16_t *offset, uint16_t *count)
{
    if (!dma_channel_is_valid(channel) || page == nullptr || offset == nullptr ||
        count == nullptr || size == 0) {
        return false;
    }

    if (address >= kIsaDmaAddressLimit) {
        return false;
    }

    if (size > kIsaDmaAddressLimit - address) {
        return false;
    }

    uint32_t dma_address = address;
    uint32_t dma_count = size;
    if (dma_channel_is_16bit(channel)) {
        if ((address & 1u) != 0 || (size & 1u) != 0) {
            return false;
        }
        dma_address >>= 1;
        dma_count >>= 1;
    }

    if (dma_count == 0 || dma_count > 0x10000u) {
        return false;
    }

    *page = static_cast<uint8_t>(address >> 16);
    *offset = dma_low16(dma_address);
    *count = static_cast<uint16_t>(dma_count - 1u);
    return true;
}

static void dma_program(uint8_t mode, uint8_t channel, uint8_t page, uint16_t offset,
                        uint16_t count)
{
    const uint8_t local_channel = channel & 0x03u;

    close_interrupt;

    outb(kDmaMaskReg[channel], static_cast<uint8_t>(0x04u | local_channel));
    outb(kDmaClearReg[channel], 0x00);
    outb(kDmaModeReg[channel], static_cast<uint8_t>(mode | local_channel));
    outb(kDmaPagePort[channel], page);

    outb(kDmaAddrPort[channel], dma_low8(offset));
    outb(kDmaAddrPort[channel], dma_high8(offset));

    outb(kDmaCountPort[channel], dma_low8(count));
    outb(kDmaCountPort[channel], dma_high8(count));

    outb(kDmaMaskReg[channel], local_channel);
    open_interrupt;
}

} // namespace

void dma_start(uint8_t mode, uint8_t channel, uint32_t *address, uint32_t size)
{
    const uint32_t physical = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address));

    uint8_t page = 0;
    uint16_t offset = 0;
    uint16_t count = 0;
    if (!dma_prepare_transfer(channel, physical, size, &page, &offset, &count)) {
        return;
    }

    dma_program(mode, channel, page, offset, count);
}

void dma_send(uint8_t channel, uint32_t *address, uint32_t size)
{
    dma_start(0x48, channel, address, size);
}

void dma_recv(uint8_t channel, uint32_t *address, uint32_t size)
{
    dma_start(0x44, channel, address, size);
}
