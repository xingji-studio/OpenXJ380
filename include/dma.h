#pragma once

#ifndef INCLUDE_DMA_H_
#    define INCLUDE_DMA_H_

#    include "stdint.h"

/*
 * Minimal ISA 8237 DMA programming helpers used by SB16 playback.
 * The interface intentionally stays narrow: callers provide a channel,
 * a physical buffer address and the transfer size in bytes.
 */
void dma_start(uint8_t mode, uint8_t channel, uint32_t *address, uint32_t size);

void dma_send(uint8_t channel, uint32_t *address, uint32_t size);
void dma_recv(uint8_t channel, uint32_t *address, uint32_t size);

#endif // INCLUDE_DMA_H_
