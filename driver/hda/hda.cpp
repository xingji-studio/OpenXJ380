#include "hda/hda.h"
#include <proto.hpp>
#include "krlibc.h"
#include <hda/vsound.h>
#include <lock_queue.h>
#include <mm/hhdm.h>
#include <pci/pci.h>
#include <cpu/longm.h>
#include <pctable/idt.h>

#define mem_geti(addr)   ({ *(volatile ssize_t *)(addr); })
#define mem_geti8(addr)  ({ *(volatile int8_t *)(addr); })
#define mem_geti16(addr) ({ *(volatile int16_t *)(addr); })
#define mem_geti32(addr) ({ *(volatile int32_t *)(addr); })
#define mem_geti64(addr) ({ *(volatile int64_t *)(addr); })

#define mem_getu(addr)   ({ *(volatile size_t *)(addr); })
#define mem_getu8(addr)  ({ *(volatile uint8_t *)(addr); })
#define mem_getu16(addr) ({ *(volatile uint16_t *)(addr); })
#define mem_getu32(addr) ({ *(volatile uint32_t *)(addr); })
#define mem_getu64(addr) ({ *(volatile uint64_t *)(addr); })

#define mem_get(addr)   mem_getu(addr)
#define mem_get8(addr)  mem_getu8(addr)
#define mem_get16(addr) mem_getu16(addr)
#define mem_get32(addr) mem_getu32(addr)
#define mem_get64(addr) mem_getu64(addr)

#define mem_set(addr, val)   ({ *(volatile size_t *)(addr) = (size_t)(val); })
#define mem_set8(addr, val)  ({ *(volatile uint8_t *)(addr) = (uint8_t)(val); })
#define mem_set16(addr, val) ({ *(volatile uint16_t *)(addr) = (uint16_t)(val); })
#define mem_set32(addr, val) ({ *(volatile uint32_t *)(addr) = (uint32_t)(val); })
#define mem_set64(addr, val) ({ *(volatile uint64_t *)(addr) = (uint64_t)(val); })
static uintptr_t hda_base;
static uintptr_t hda_output_base;
static uint32_t *hda_output_buffer;
static uint64_t  hda_output_buffer_phys = 0;
static uint32_t *corb             = NULL;
static uint32_t *rirb             = NULL;
static uint64_t  corb_phys        = 0;
static uint64_t  rirb_phys        = 0;
static uint32_t  corb_entry_count = 0, corb_write_pointer = 0;
static uint32_t  rirb_entry_count = 0, rirb_read_pointer = 0;
static uint32_t  send_verb_method;
static uint32_t  hda_codec_number          = 0;
static uint32_t  hda_afg_pcm_format_cap    = 0;
static uint32_t  hda_afg_stream_format_cap = 0;
static uint32_t  hda_afg_amp_cap           = 0;
static uint32_t  hda_pin_output_node       = 0;
static uint32_t  hda_pin_output_amp_cap    = 0;
static uint32_t  hda_pin_pcm_format_cap    = 0;
static uint32_t  hda_pin_stream_format_cap = 0;
static vsound_t  snd;
static void     *hda_buffer_ptr = NULL;
static uint64_t  hda_buffer_phys = 0;
static bool      hda_stopping   = false;
static pcb_t    use_task;

static constexpr uint32_t HDA_WAIT_STEP_US           = 50;
static constexpr uint32_t HDA_MMIO_VERB_TIMEOUT_US   = 20000;
static constexpr uint32_t HDA_DMA_VERB_TIMEOUT_US    = 20000;
static constexpr uint32_t HDA_CONTROLLER_RESET_US    = 20000;
static constexpr uint32_t HDA_RING_RESET_TIMEOUT_US  = 20000;

static bool hda_wait_reg16(uintptr_t addr, uint16_t mask, uint16_t expected, uint32_t timeout_us) {
    for (uint32_t waited = 0; waited <= timeout_us; waited += HDA_WAIT_STEP_US) {
        if ((mem_get16(addr) & mask) == expected) return true;
        if (waited < timeout_us) delay_us_hp(HDA_WAIT_STEP_US);
    }
    return false;
}

static bool hda_wait_reg32(uintptr_t addr, uint32_t mask, uint32_t expected, uint32_t timeout_us) {
    for (uint32_t waited = 0; waited <= timeout_us; waited += HDA_WAIT_STEP_US) {
        if ((mem_get32(addr) & mask) == expected) return true;
        if (waited < timeout_us) delay_us_hp(HDA_WAIT_STEP_US);
    }
    return false;
}

// HDA 控制器只能直接访问物理地址，这里统一申请 32 位地址空间内的 DMA 缓冲。
static void *hda_alloc_dma32(size_t size, uint64_t *phys_out) {
    const size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t     phys       = alloc_frames_dma32(page_count);
    if (phys == 0) return NULL;

    void *virt = phys_to_virt(phys);
    memset(virt, 0, page_count * PAGE_SIZE);
    if (phys_out) *phys_out = phys;
    return virt;
}

static int hda_open(vsound_t vsound) {
    write_serial_fmt("hda open has been called");
    use_task = get_current_task()->parent_group;
    return 0;
}

static void hda_close(vsound_t snd) {
    if (!snd->is_dma_ready) {
        hda_stopping = true;
    } else {
        hda_stop();
    }
}

static int hda_start_dma(vsound_t snd, void *addr) {
    if (snd->is_dma_ready) return 0;
    hda_play_pcm(addr, HDA_BUF_SIZE, snd->rate, snd->channels, sound_fmt_bytes((sound_pcmfmt_t)snd->fmt) * 8);
    return 0;
}

void hda_stop() {
    mem_set8(hda_output_base + 0x0, 0);
}

void hda_continue() {
    mem_set8(hda_output_base + 0x0, 0b110);
}

uint32_t hda_get_bytes_sent() {
    return mem_get32(hda_output_base + 0x4);
}

// 中断与主动轮询共用这段收尾逻辑，避免缓冲区回收路径分叉。
static void hda_handle_buffer_completion() {
    uint8_t status = mem_get8(hda_output_base + 0x3);
    if ((status & (1 << 2)) == 0) return;

    mem_set8(hda_output_base + 0x3, 1 << 2);
    if (hda_stopping) {
        hda_stop();
    } else {
        vsound_played(snd);
    }
}

void hda_poll() {
    if (hda_output_base == 0) return;
    hda_handle_buffer_completion();
}

uint8_t hda_node_type(uint32_t codec, uint32_t node) {
    return (hda_verb(codec, node, 0xf00, 0x9) >> 20) & 0xf;
}

uint8_t hda_pin_complex_type(uint32_t codec, uint32_t node) {
    return (hda_verb(codec, node, 0xF1C, 0) >> 20) & 0xf;
}

uint16_t hda_get_connection_list_entry(uint32_t codec, uint32_t node, uint32_t index) {
    uint32_t cll        = hda_verb(codec, node, 0xf00, 0xe);
    bool     long_entry = (cll & 0x80) != 0;
    write_serial_fmt("cll: %08x and long_entry: %d\n", cll, long_entry);
    if (!long_entry) {
        return (hda_verb(codec, node, 0xf02, index / 4 * 4) >> ((index % 4) * 8)) & 0xff;
    } else {
        return (hda_verb(codec, node, 0xf02, index / 2 * 2) >> ((index % 2) * 16)) & 0xffff;
    }
    __builtin_unreachable();
    return 0;
}

void hda_pin_set_output_volume(uint32_t codec, uint32_t node, uint32_t cap, uint32_t volume) {
    uint32_t val  = (1 << 12) | (1 << 13);
    val          |= 0x8000;
    if (volume == 0 && cap & 0x80000000) {
        val |= (1 << 7);
    } else {
        val |= ((cap >> 8) & 0x7F) * volume / 100;
    }
    hda_verb(codec, node, 0x3, val);
}

void hda_init_audio_output(uint32_t codec, uint32_t node) {
    hda_verb(codec, node, 0x705, 0x0);
    hda_verb(codec, node, 0x708, 0x0);
    hda_verb(codec, node, 0x703, 0x0);
    hda_verb(codec, node, 0x706, 0x10);

    hda_pin_output_node = node;

    uint32_t amp_cap = hda_verb(codec, node, 0xf00, 0x12);
    hda_pin_set_output_volume(codec, node, amp_cap, 100);
    if (amp_cap != 0) {
        hda_pin_output_node    = node;
        hda_pin_output_amp_cap = amp_cap;
    }
    uint32_t pcm_format_cap = hda_verb(codec, node, 0xf00, 0xA);
    write_serial_fmt("pcm format cap: %08x\n", pcm_format_cap);
    if (pcm_format_cap != 0) {
        hda_pin_pcm_format_cap = pcm_format_cap;
    } else {
        hda_pin_pcm_format_cap = hda_afg_pcm_format_cap;
        write_serial_fmt("using afg pcm format cap: %08x\n", hda_afg_pcm_format_cap);
    }

    uint32_t stream_format_cap = hda_verb(codec, node, 0xf00, 0xB);
    if (stream_format_cap != 0) {
        hda_pin_stream_format_cap = stream_format_cap;
    } else {
        hda_pin_stream_format_cap = hda_afg_stream_format_cap;
    }
    if (hda_pin_output_amp_cap == 0) {
        hda_pin_output_node    = node;
        hda_pin_output_amp_cap = hda_afg_amp_cap;
    }
    write_serial_fmt("successfully initialized audio output node %d S\n", hda_pin_output_node);
}

void hda_init_audio_mixer(uint32_t codec, uint32_t node) {
    hda_verb(codec, node, 0x705, 0x0);
    hda_verb(codec, node, 0x708, 0x0);

    uint32_t amp_cap = hda_verb(codec, node, 0xf00, 0x12);
    hda_pin_set_output_volume(codec, node, amp_cap, 100);
    if (amp_cap != 0) {
        hda_pin_output_node    = node;
        hda_pin_output_amp_cap = amp_cap;
    }
    uint16_t n    = hda_get_connection_list_entry(codec, node, 0);
    uint8_t  type = hda_node_type(codec, n);
    if (type == HDA_WIDGET_AUDIO_OUTPUT) {
        write_serial_fmt("(%08x) : node %d is Audio Output\n", type, n);
        hda_init_audio_output(codec, n);
    } else if (type == HDA_WIDGET_MIXER) {
        write_serial_fmt("(%08x) : node %d is Mixer\n", type, n);
        hda_init_audio_mixer(codec, n);
    } else if (type == HDA_WIDGET_AUDIO_SELECTOR) {
        write_serial_fmt("(%08x) : node %d is Audio Selector\n", type, n);
        hda_init_audio_selector(codec, n);
    }
}

void hda_init_audio_selector(uint32_t codec, uint32_t node) {
    hda_verb(codec, node, 0x705, 0x0);
    hda_verb(codec, node, 0x708, 0x0);
    hda_verb(codec, node, 0x703, 0x0);

    uint32_t amp_cap = hda_verb(codec, node, 0xf00, 0x12);
    hda_pin_set_output_volume(codec, node, amp_cap, 100);
    if (amp_cap != 0) {
        hda_pin_output_node    = node;
        hda_pin_output_amp_cap = amp_cap;
    }
    uint16_t n    = hda_get_connection_list_entry(codec, node, 0);
    uint8_t  type = hda_node_type(codec, n);
    if (type == HDA_WIDGET_AUDIO_OUTPUT) {
        write_serial_fmt("(%08x) : node %d is Audio Output\n", type, n);
        hda_init_audio_output(codec, n);
    }
}

uint16_t hda_return_sound_data_format(uint32_t sample_rate, uint32_t channels,
                                      uint32_t bits_per_sample) {
    uint16_t data_format = 0;

    // channels
    data_format = (channels - 1);

    // bits per sample
    switch (bits_per_sample) {
    case 16: data_format |= (0b001 << 4); break;
    case 20: data_format |= (0b010 << 4); break;
    case 24: data_format |= (0b011 << 4); break;
    case 32: data_format |= (0b100 << 4); break;
    default: /* Handle invalid bits_per_sample if necessary */ break;
    }

    // sample rate
    switch (sample_rate) {
    case 48000: data_format |= (0b0000000 << 8); break;
    case 44100: data_format |= (0b1000000 << 8); break;
    case 32000: data_format |= (0b0001010 << 8); break;
    case 22050: data_format |= (0b1000001 << 8); break;
    case 16000: data_format |= (0b0000010 << 8); break;
    case 11025: data_format |= (0b1000011 << 8); break;
    case 8000: data_format |= (0b0000101 << 8); break;
    case 88200: data_format |= (0b1001000 << 8); break;
    case 96000: data_format |= (0b0001000 << 8); break;
    case 176400: data_format |= (0b1011000 << 8); break;
    case 192000: data_format |= (0b0011000 << 8); break;
    default: /* Handle invalid sample_rate if necessary */ break;
    }

    return data_format;
}

uint8_t hda_is_supported_sample_rate(uint32_t sample_rate) {
    uint32_t sample_rates[11] = {8000,  11025, 16000, 22050,  32000, 44100,
                                 48000, 88200, 96000, 176400, 192000};
    uint16_t mask             = 0x0000001;
    // get bit of requested sample rate in capabilities
    for (int i = 0; i < 11; i++) {
        if (sample_rates[i] == sample_rate) { break; }
        mask <<= 1;
    }

    if ((hda_pin_pcm_format_cap & mask) == mask) {
        return 1;
    } else {
        return 0;
    }
}
void explicit_bzero(void *_s, size_t _n) {
    for (size_t i = 0; i < _n; i++) {
        ((uint8_t *)_s)[i] = 0;
    }
}

void hda_play_pcm(void *buffer, uint32_t size, uint32_t sample_rate, uint32_t channels,
                  uint32_t bits_per_sample) {
    // BDL 中写入的必须是物理地址，直接写虚拟地址会导致控制器 DMA 到错误位置。
    const uint64_t buffer_phys = (uint64_t)virt_to_phys((uint64_t)buffer);
    uint16_t data_format = hda_return_sound_data_format(sample_rate, channels, bits_per_sample);
    if (!(hda_pin_stream_format_cap & 1)) {
        write_serial_fmt("pcm format not supported\n");
        return;
    }
    mem_set8(hda_output_base + 0x0, 0);
    delay_ms_hp(250);
    if ((mem_get8(hda_output_base + 0x0) & 0b11) != 0x0) {
        write_serial_fmt("output reset failed 1\n");
        return;
    }
    mem_set8(hda_output_base + 0x0, 1);
    delay_ms_hp(250);
    if ((mem_get8(hda_output_base + 0x0) & 0b01) != 0x1) {
        write_serial_fmt("stream reset failed 2\n");
        return;
    }
    delay_ms_hp(250);

    mem_set8(hda_output_base + 0x0, 0);
    delay_ms_hp(250);
    if ((mem_get8(hda_output_base + 0x0) & 0b11) != 0x0) {
        write_serial_fmt("output reset failed\n");
        return;
    }
    delay_ms_hp(250);

    mem_set8(hda_output_base + 0x3, 0b11100);
    explicit_bzero(hda_output_buffer, 16 * 2);
    hda_output_buffer[0] = (uint32_t)(buffer_phys & 0xffffffffu);
    hda_output_buffer[1] = (uint32_t)(buffer_phys >> 32);
    hda_output_buffer[2] = size;
    hda_output_buffer[3] = 1;

    hda_output_buffer[4] = (uint32_t)((buffer_phys + size) & 0xffffffffu);
    hda_output_buffer[5] = (uint32_t)((buffer_phys + size) >> 32);
    hda_output_buffer[6] = size;
    hda_output_buffer[7] = 1;

    __asm__ volatile("wbinvd");

    mem_set32(hda_output_base + 0x18, (uint32_t)(hda_output_buffer_phys & 0xffffffffu));
    mem_set32(hda_output_base + 0x1c, (uint32_t)(hda_output_buffer_phys >> 32));

    mem_set32(hda_output_base + 0x8, size * 2);
    mem_set16(hda_output_base + 0xc, 1);
    mem_set16(hda_output_base + 0x12, data_format);
    // printf("data_format = %x %d\n", data_format, hda_pin_output_node);
    hda_verb(hda_codec_number, hda_pin_output_node, 0x2, data_format);
    delay_ms_hp(250);

    mem_set8(hda_output_base + 0x2, 0x1c);

    mem_set8(hda_output_base + 0x0, 0b110);
}

void hda_init_output_pin(uint32_t codec, uint32_t node) {
    hda_verb(codec, node, 0x705, 0x0);
    hda_verb(codec, node, 0x708, 0x0);
    hda_verb(codec, node, 0x703, 0x0);

    hda_verb(codec, node, 0x707, hda_verb(codec, node, 0xf07, 0) | (1 << 6) | (1 << 7));
    hda_verb(codec, node, 0x70c, 1);

    uint32_t amp_cap = hda_verb(codec, node, 0xf00, 0x12);
    hda_pin_set_output_volume(codec, node, amp_cap, 100);
    if (amp_cap != 0) {
        hda_pin_output_node    = node;
        hda_pin_output_amp_cap = amp_cap;
    }
    hda_verb(codec, node, 0x701, 0);
    uint16_t n    = hda_get_connection_list_entry(codec, node, 0);
    uint8_t  type = hda_node_type(codec, n);
    if (type == HDA_WIDGET_AUDIO_OUTPUT) {
        write_serial_fmt("(%08x) : node %d is Audio Output\n", type, n);
        hda_init_audio_output(codec, n);
    } else if (type == HDA_WIDGET_MIXER) {
        write_serial_fmt("(%08x) : node %d is Mixer\n", type, n);
        hda_init_audio_mixer(codec, n);
    } else if (type == HDA_WIDGET_AUDIO_SELECTOR) {
        write_serial_fmt("(%08x) : node %d is Audio Selector\n", type, n);
        hda_init_audio_selector(codec, n);
    }
}

void hda_init_afg(uint32_t codec, uint32_t node) {
    hda_verb(codec, node, 0x7ff, 0);
    hda_verb(codec, node, 0x705, 0);
    hda_verb(codec, node, 0x708, 0);

    hda_afg_pcm_format_cap    = hda_verb(codec, node, 0xF00, 0xA);
    hda_afg_stream_format_cap = hda_verb(codec, node, 0xF00, 0xB);
    hda_afg_amp_cap           = hda_verb(codec, node, 0xF00, 0x12);

    write_serial_fmt("pcm format cap: %08x\n", hda_afg_pcm_format_cap);
    write_serial_fmt("stream format cap: %08x\n", hda_afg_stream_format_cap);
    write_serial_fmt("amp cap: %08x\n", hda_afg_amp_cap);

    uint32_t count_raw  = hda_verb(codec, node, 0xF00, 0x4);
    uint32_t node_start = (count_raw >> 16) & 0xff;
    uint32_t node_count = count_raw & 0xff;
    write_serial_fmt("(%08x) : node %d has %d subnodes from %d\n", count_raw, node, node_count, node_start);
    uint32_t pin_speaker   = 0;
    uint32_t pin_headphone = 0;
    uint32_t pin_output    = 0;
    for (int i = node_start; i < node_start + node_count; i++) {
        uint8_t type = hda_node_type(codec, i);
        if (type == HDA_WIDGET_AUDIO_OUTPUT) {
            write_serial_fmt("(%08x) : node %d is Audio Output\n", type, i);
            hda_verb(codec, i, 0x706, 0x0);
        }
        if (type == HDA_WIDGET_PIN_COMPLEX) {
            write_serial_fmt("(%08x) : node %d is Pin Complex\n", type, i);
            uint8_t pin_type = hda_pin_complex_type(codec, i);
            write_serial_fmt("(%08x) : pin type is %d\n", pin_type, i);
            if (pin_type == HDA_PIN_COMPLEX_LINE_OUT) {
                write_serial_fmt("(%08x) : node %d is Line Out\n", pin_type, i);
                pin_output = i;
            } else if (pin_type == HDA_PIN_COMPLEX_SPEAKER) {
                write_serial_fmt("(%08x) : node %d is Speaker\n", pin_type, i);
                if (pin_speaker == 0) pin_speaker = i;
                if ((hda_verb(codec, i, 0xf00, 0x09) & 0b100) &&
                    (hda_verb(codec, i, 0xf1c, 0) >> 30 & 0b11) != 1 &&
                    (hda_verb(codec, i, 0xf00, 0x0c) & 0b10000)) {
                    write_serial_fmt("the speaker is connected ready to output\n");
                    pin_speaker = i;
                } else {
                    write_serial_fmt("no output\n");
                }
            } else if (pin_type == HDA_PIN_COMPLEX_HP_OUT) {
                write_serial_fmt("(%08x) : node %d is Headphone\n", pin_type, i);
                pin_headphone = i;
            } else if (pin_type == HDA_PIN_COMPLEX_CD) {

                write_serial_fmt("(%08x) : node %d is CD\n", pin_type, i);
                pin_output = i;
            } else if (pin_type == HDA_PIN_COMPLEX_SPDIF_OUT) {
                write_serial_fmt("(%08x) : node %d is SPDIF Out\n", pin_type, i);
                pin_output = i;
            } else if (pin_type == HDA_PIN_COMPLEX_DIG_OUT) {
                write_serial_fmt("(%08x) : node %d is Digital Out\n", pin_type, i);
                pin_output = i;
            } else if (pin_type == HDA_PIN_COMPLEX_MODEM_LINE_SIDE) {
                write_serial_fmt("(%08x) : node %d is Modem Line Side\n", pin_type, i);
                pin_output = i;
            } else if (pin_type == HDA_PIN_COMPLEX_MODEM_HANDSET_SIDE) {
                write_serial_fmt("(%08x) : node %d is Modem Handset Side\n", pin_type, i);

                pin_output = i;
            }
        }
    }
    write_serial_fmt("pin_speaker: %d, pin_headphone: %d, pin_output: %d\n", pin_speaker, pin_headphone,
          pin_output);
    if (pin_speaker) {
        hda_init_output_pin(codec, pin_speaker);
        if (!pin_headphone) return;
        // TODO: 处理耳机
    } else if (pin_headphone) {
        hda_init_output_pin(codec, pin_headphone);
    } else if (pin_output) {
        hda_init_output_pin(codec, pin_output);
    }
}

uint32_t hda_verb(uint32_t codec, uint32_t node, uint32_t verb, uint32_t command) {
    if (verb == 0x2 || verb == 0x3) { verb <<= 8; }
    uint32_t value = ((codec << 28) | (node << 20) | (verb << 8) | (command));
    if (send_verb_method == SEND_VERB_METHOD_DMA) {
        corb[corb_write_pointer] = value;
        mem_set16(hda_base + 0x48, corb_write_pointer);
        if (!hda_wait_reg16(hda_base + 0x58, 0xff, (uint16_t)corb_write_pointer, HDA_DMA_VERB_TIMEOUT_US)) {
            write_serial_fmt("hda_verbI: No response from hda.\n");
            return 0;
        }
        value              = rirb[corb_write_pointer * 2];
        corb_write_pointer = (corb_write_pointer + 1) % corb_entry_count;
        rirb_read_pointer  = (rirb_read_pointer + 1) % rirb_entry_count;
        return value;
    } else {
        mem_set16(hda_base + 0x68, 0b10);
        mem_set32(hda_base + 0x60, value);
        mem_set16(hda_base + 0x68, 1);
        if (!hda_wait_reg16(hda_base + 0x68, 0x3, 0b10, HDA_MMIO_VERB_TIMEOUT_US)) {
            mem_set16(hda_base + 0x68, 0b10);
            write_serial_fmt("hda_verbII: No response from hda.\n");
            return 0;
        } else {
            mem_set16(hda_base + 0x68, 0b10);
        }
        return mem_get32(hda_base + 0x64);
    }
}

void hda_init_codec(uint32_t codec) {
    uint32_t count_raw  = hda_verb(codec, 0, 0xf00, 0x4);
    uint32_t node_start = (count_raw >> 16) & 0xff;
    uint32_t node_count = count_raw & 0xff;
    write_serial_fmt("(%x) : codec %d has %d nodes from %d\n", count_raw, codec, node_count, node_start);
    for (int i = node_start; i < node_start + node_count; i++) {
        uint32_t type = hda_verb(codec, i, 0xf00, 0x5);
        if ((type & 0xff) == 0x1) {
            write_serial_fmt("(%x) : node %d is Audio Function Group\n", type, i);
            hda_init_afg(codec, i);
            return;
        }
    }
}

extern "C" void hda_interrupt_handler(struct X64_REGS *frame, uint64_t error_code) {
    write_serial_fmt("HDA IRQ\n");
    hda_handle_buffer_completion();
    __asm__("wbinvd");
    send_eoi();
}
extern uint32_t pci_read0(uint32_t b, uint32_t d, uint32_t f, uint32_t arg, uint32_t registeroffset);
void pci_write0(uint32_t b, uint32_t d, uint32_t f, uint32_t arg, uint32_t registeroffset, uint32_t value);
extern uint32_t read_bar_n(pci_device_t *device, uint8_t bar_n);
uint8_t pci_get_drive_irq(uint8_t bus, uint8_t slot, uint8_t func) {
    return (uint8_t)pci_read0(bus, slot, func, NULL,0x3c);
}
extern void init_idt_desc(uint8_t number, uint8_t gate_type, int_func function, uint8_t privilege, void *data);
void hda_init() {
    pci_device_t *device = pci_find_vid_did(0x8086, 0x2668);
    if (device == NULL) {
        device = pci_find_vid_did(0x8086, 0x27d8);
        if (device == NULL) device = pci_find_vid_did(0x1002, 0x4383);
        if (device == NULL) device = pci_find_vid_did(0x8086, 0x293e);
        if (device == NULL) device = pci_find_class(0x040300);
        if (device == NULL) device = pci_find_class(0x040100);
        if (device == NULL) {
            write_serial_fmt("Cannot find a High Definition Audio controller.\n");
            return;
        }
    }
    write_serial_fmt( "Loading Intel High Definition Audio driver...\n");

    // 打开中断 | 启用总线主控 | 启用MMIO
    uint32_t d = pci_read0(device->bus, device->slot, device->func, NULL,0x04);
    pci_write0(device->bus, device->slot, device->func, NULL, 0x04,
              ((d & ~((uint32_t)1 << 10)) | (1 << 2) | (1 << 1)));

    pci_bar_base_address bar0 = device->bars[0];
    if (!bar0.mmio || bar0.address == 0) {
        write_serial_fmt("HDA controller BAR0 is invalid.\n");
        return;
    }
    hda_base = (uintptr_t)phys_to_virt(bar0.address);

    mem_set32(hda_base + 0x08, 0);
    if (!hda_wait_reg32(hda_base + 0x08, 0x01, 0, HDA_CONTROLLER_RESET_US)) {
        write_serial_fmt("Intel High Definition Audio reset failed.\n");
        return;
    }
    mem_set32(hda_base + 0x08, 1);
    if (!hda_wait_reg32(hda_base + 0x08, 0x01, 1, HDA_CONTROLLER_RESET_US)) {
        write_serial_fmt("Intel High Definition Audio reset failed.\n");
        return;
    }

    write_serial_fmt("hda card is working now\n");

    int input_stream_count = (mem_get16(hda_base + 0x00) >> 8) & 0x0f;
    write_serial_fmt("input stream count: %d\n", input_stream_count);
    hda_output_base   = hda_base + 0x80 + (0x20 * input_stream_count);
    // BDL 和双缓冲都要放在 DMA 可见的地址范围内，否则控制器无法访问。
    hda_output_buffer = (uint32_t *)hda_alloc_dma32(PAGE_SIZE, &hda_output_buffer_phys);
    hda_buffer_ptr    = hda_alloc_dma32(HDA_BUF_SIZE * 2, &hda_buffer_phys);
    if (hda_output_buffer == NULL || hda_buffer_ptr == NULL) {
        write_serial_fmt("Failed to allocate HDA DMA buffers.\n");
        return;
    }

    int irq = pci_get_drive_irq(device->bus, device->slot, device->func);
    write_serial_fmt("HDA irq %d\n",0x20+irq);
    init_idt_desc(0x20+irq,X86_64_INTR_GATE,hda_interrupt,RING0,NULL);
    mem_set32(hda_base + 0x20, ((uint32_t)1 << 31) | ((uint32_t)1 << input_stream_count));
    uint8_t  rirb_size  = 0;
    uint32_t corb_size  = 0;

    mem_set32(hda_base + 0x70, 0);
    mem_set32(hda_base + 0x74, 0);
    mem_set32(hda_base + 0x34, 0);
    mem_set32(hda_base + 0x38, 0);
    mem_set8(hda_base + 0x4c, 0);
    mem_set8(hda_base + 0x5c, 0);
    write_serial_fmt("Using immediate command mode for HDA verbs.\n");
    goto mmio;

    corb = (uint32_t *)hda_alloc_dma32(PAGE_SIZE, &corb_phys);
    if (corb == NULL) {
        write_serial_fmt("Failed to allocate HDA CORB.\n");
        goto mmio;
    }
    mem_set32(hda_base + 0x40, (uint32_t)(corb_phys & 0xffffffffu));
    mem_set32(hda_base + 0x44, (uint32_t)(corb_phys >> 32));
    corb_size = mem_get8(hda_base + 0x4e) >> 4 & 0x0f;
    if (corb_size & 0b0001) {
        corb_entry_count = 2;
        mem_set8(hda_base + 0x4e, 0b00);
    } else if (corb_size & 0b0010) {
        corb_entry_count = 16;
        mem_set8(hda_base + 0x4e, 0b01);
    } else if (corb_size & 0b0100) {
        corb_entry_count = 256;
        mem_set8(hda_base + 0x4e, 0b10);
    } else {
        write_serial_fmt("HDA corb size not supported.\n");
        goto mmio;
    }
    write_serial_fmt("corb size: %d entries\n", corb_entry_count);

    mem_set16(hda_base + 0x4A, 0x8000);
    if (!hda_wait_reg16(hda_base + 0x4A, 0x8000, 0x8000, HDA_RING_RESET_TIMEOUT_US)) {
        write_serial_fmt("HDA corb reset failed\n");
        goto mmio;
    }
    mem_set16(hda_base + 0x4A, 0x0000);
    if (!hda_wait_reg16(hda_base + 0x4A, 0x8000, 0, HDA_RING_RESET_TIMEOUT_US)) {
        write_serial_fmt("HDA corb reset failed\n");
        goto mmio;
    }
    mem_set16(hda_base + 0x48, 0);
    corb_write_pointer = 1;
    write_serial_fmt("corb has been reset already\n");

    rirb = (uint32_t *)hda_alloc_dma32(PAGE_SIZE, &rirb_phys);
    if (rirb == NULL) {
        write_serial_fmt("Failed to allocate HDA RIRB.\n");
        goto mmio;
    }
    mem_set32(hda_base + 0x50, (uint32_t)(rirb_phys & 0xffffffffu));
    mem_set32(hda_base + 0x54, (uint32_t)(rirb_phys >> 32));

    rirb_size = mem_get8(hda_base + 0x5e) >> 4 & 0x0f;
    if (rirb_size & 0b0001) {
        rirb_entry_count = 2;
        mem_set8(hda_base + 0x5e, 0b00);
    } else if (rirb_size & 0b0010) {
        rirb_entry_count = 16;
        mem_set8(hda_base + 0x5e, 0b01);
    } else if (rirb_size & 0b0100) {
        rirb_entry_count = 256;
        mem_set8(hda_base + 0x5e, 0b10);
    } else {
        write_serial_fmt("HDA rirb size not supported\n");
        goto mmio;
    }
    write_serial_fmt("rirb size: %d entries\n", rirb_entry_count);

    mem_set16(hda_base + 0x58, 0x8000);
    if (!hda_wait_reg16(hda_base + 0x58, 0x8000, 0x8000, HDA_RING_RESET_TIMEOUT_US)) {
        write_serial_fmt("HDA rirb reset failed\n");
        goto mmio;
    }
    mem_set16(hda_base + 0x5A, 0x0000);
    rirb_read_pointer = 1;
    write_serial_fmt("rirb has been reset already\n");

    mem_set8(hda_base + 0x4c, 0b10);
    mem_set8(hda_base + 0x5c, 0b10);
    write_serial_fmt("corb rirb dma has been started\n");
    send_verb_method = SEND_VERB_METHOD_DMA;
    for (int i = 0; i < 16; i++) {
        uint32_t codec_id = hda_verb(i, 0, 0xf00, 0);
        if (codec_id != 0) {
            hda_codec_number = i;
            hda_init_codec(i);
            if (hda_pin_output_node == 0) {
                write_serial_fmt("HDA codec detected but no output node was initialized.\n");
                return;
            }
            hda_regist();
            write_serial_fmt("Intel High Definition Audio load done!\n");
            return;
        }
    }

mmio:
    send_verb_method = SEND_VERB_METHOD_MMIO;
    mem_set8(hda_base + 0x4c, 0);
    mem_set8(hda_base + 0x5c, 0);
    for (int i = 0; i < 16; i++) {
        uint32_t codec_id = hda_verb(i, 0, 0xf00, 0);
        if (codec_id != 0) {
            hda_codec_number = i;
            hda_init_codec(i);
            if (hda_pin_output_node == 0) {
                write_serial_fmt("HDA codec detected but no output node was initialized.\n");
                return;
            }
            hda_regist();
            write_serial_fmt( "Intel High Definition Audio load done!\n");
            return;
        }
    }
}

static struct vsound vsound = {
    .is_output = true,
    .name      = "hda",
    .open      = hda_open,
    .close     = hda_close,
    .start_dma = hda_start_dma,
    .bufsize   = HDA_BUF_SIZE,
};

static const int16_t fmts[] = {
    SOUND_FMT_S16, SOUND_FMT_U16, SOUND_FMT_U32, SOUND_FMT_S32, -1,
};

static const int32_t rates[] = {
    8000, 11025, 16000, 22050, 24000, 32000, 44100, 47250, 48000, 50000, -1,
};

void hda_regist() {
    snd = &vsound;
    if (snd->is_registed) return;
    // vsound 只负责注册，缓冲队列需要驱动自己准备好。
    snd->bufs0 = queue_init();
    snd->bufs1 = queue_init();
    if (snd->bufs0 == NULL || snd->bufs1 == NULL || hda_buffer_ptr == NULL) {
        write_serial_fmt("register hda fault");
        return;
    }
    if (!vsound_regist(snd)) {
        write_serial_fmt("register hda fault");
        return;
    }
    vsound_set_supported_fmts(snd, fmts, -1);
    vsound_set_supported_rates(snd, rates, -1);
    vsound_set_supported_ch(snd, 1);
    vsound_set_supported_ch(snd, 2);
    vsound_addbuf(snd, hda_buffer_ptr);
    // 第二块缓冲区紧跟第一块之后，按字节偏移一个完整的 HDA_BUF_SIZE。
    vsound_addbuf(snd, (uint8_t *)hda_buffer_ptr + HDA_BUF_SIZE);
}
