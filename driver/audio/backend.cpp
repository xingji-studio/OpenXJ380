// SPDX-License-Identifier: Apache-2.0
#include <audio_backend.h>
#include <hda/hda.h>
#include <proto.hpp>

static const AudioBackend *selected;
static const AudioBackend  hda_backend = {
    "Intel HDA shared output", 48000,           hda_audio_start,      hda_audio_stop,           hda_audio_progress,
    hda_audio_refill,          hda_audio_abort, hda_audio_set_worker, hda_audio_interrupt_count};

void audio_backend_init()
{
    hda_init();
    if (hda_audio_available()) selected = &hda_backend;
    if (!selected) selected = ac97_probe();
    if (!selected) selected = es1371_probe();
    if (!selected) selected = sb16_probe();
    if (selected)
        write_serial_fmt("audio: selected %s rate=%u periods=%u frames=%u\n", selected->name, selected->rate,
                         AUDIO_OUTPUT_PERIODS, AUDIO_OUTPUT_FRAMES);
    else
        write_serial_string("audio: no supported playback controller\n");
}

const AudioBackend *audio_backend()
{
    return selected;
}
