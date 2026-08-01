#pragma once

#include <efi/fbc.h>
#include <stdint.h>

void fbdev_setup(const FrameBufferConfig &fbc);
bool fbdev_ready();
uint8_t *fbdev_active_framebuffer();
uint8_t *fbdev_kernel_framebuffer();
void fbdev_present_region(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);
void fbdev_present_kernel_region(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);
