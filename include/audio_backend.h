// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

struct thread_control_block;

// One selected stereo S16 output. Completion counts refer to whole hardware
// periods since Start; Refill(n) supplies the storage retired by completion n.
constexpr unsigned AUDIO_OUTPUT_PERIODS = 8;
constexpr unsigned AUDIO_OUTPUT_FRAMES  = 1024;
struct AudioBackend
{
    const char *name;
    uint32_t    rate;
    int (*start)();
    void (*stop)();
    int (*progress)(uint64_t *completed);
    int (*refill)(uint64_t completed, const int16_t *pcm);
    void (*abort)();
    void (*set_worker)(thread_control_block *worker);
    uint64_t (*interrupt_count)();
};

void                audio_backend_init();
const AudioBackend *audio_backend();
const AudioBackend *ac97_probe();
const AudioBackend *es1371_probe();
const AudioBackend *sb16_probe();
