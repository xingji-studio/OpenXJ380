#pragma once

#include <efi/fbc.h>

void console_init(const FrameBufferConfig &fbc);
void console_write(const char *str);
