#pragma once

#include "bitmap.h"
#include "efi/boot.h"
#include "stdint.h"

typedef struct
{
    Bitmap bitmap;
    size_t origin_frames;
    size_t usable_frames;
} FrameAllocator;

extern FrameAllocator frame_allocator;
