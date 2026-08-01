#pragma once

#include <stdint.h>

void dma_start(uint8_t mode, uint8_t channel, uint32_t *address, uint32_t size);
void dma_send(uint8_t channel, uint32_t *address, uint32_t size);
void dma_recv(uint8_t channel, uint32_t *address, uint32_t size);
