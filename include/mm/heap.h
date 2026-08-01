#pragma once

#define ALIGNED_BASE 0x1000

#include <mm/alloc/alloc.h>

uint64_t get_all_memusage();
void     init_heap();
bool     kernel_heap_extend(size_t min_bytes);
bool     kernel_heap_ready();
