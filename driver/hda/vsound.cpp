#include "hda/vsound.h"
#include "krlibc.h"
#include "proto.hpp"

#define ALL_IMPLEMENTATION
#include "rbtree-strptr.h"

static rbtree_sp_t vsound_list;

#define MASK32(n) ((uint32_t)1 << (n))

static int samplerate_id(int rate) {
    switch (rate) {
    case 8000: return 0;
    case 11025: return 1;
    case 16000: return 2;
    case 22050: return 3;
    case 24000: return 4;
    case 32000: return 5;
    case 44100: return 6;
    case 47250: return 7;
    case 48000: return 8;
    case 50000: return 9;
    case 88200: return 10;
    case 96000: return 11;
    case 176400: return 12;
    case 192000: return 13;
    case 352800: return 14;
    case 384000: return 15;
    case 768000: return 16;
    default: return -1;
    }
}

// 大小
int _sound_fmt_bytes[] = {
    // - 8bit / 1byte
    1,
    1,
    // - 16bit / 2byte
    2,
    2,
    2,
    2,
    // - 24bit / 3byte
    3,
    3,
    3,
    3,
    // - 24bit / 4byte(low 3byte)
    4,
    4,
    4,
    4,
    // - 32bit / 4byte
    4,
    4,
    4,
    4,
    // - 64bit / 8byte
    8,
    8,
    8,
    8,
    // - 16bit / 2byte  <- float
    2,
    2,
    // - 32bit / 4byte  <- float
    4,
    4,
    // - 64bit / 8byte  <- float
    8,
    8,
};

int sound_fmt_bytes(sound_pcmfmt_t fmt) {
    if (fmt < 0 || fmt >= SOUND_FMT_CNT) return -1;
    return _sound_fmt_bytes[fmt];
}

bool vsound_regist(vsound_t device) {
    if (device == NULL) return false;
    if (device->is_registed || device->is_using) return false;
    if (rbtree_sp_get(vsound_list, device->name)) return false;
    rbtree_sp_insert(vsound_list, device->name, device);
    device->is_registed = true;
    return true;
}

bool vsound_set_supported_fmts(vsound_t device, const int16_t *fmts, ssize_t len) {
    if (device == NULL) return false;
    size_t nseted = 0;
    if (len < 0) {
        for (size_t i = 0; fmts[i] >= 0; i++) {
            if (fmts[i] >= SOUND_FMT_CNT) {
                write_serial_fmt("不支持的采样格式 %d", fmts[i]);
                continue;
            }
            device->supported_fmts |= MASK32(fmts[i]);
            nseted++;
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            if (fmts[i] >= SOUND_FMT_CNT) {
                write_serial_fmt("不支持的采样格式 %d", fmts[i]);
                continue;
            }
            device->supported_fmts |= MASK32(fmts[i]);
            nseted++;
        }
    }
    return nseted > 0;
}

bool vsound_set_supported_rates(vsound_t device, const int32_t *rates, ssize_t len) {
    if (device == NULL) return false;
    size_t nseted = 0;
    if (len < 0) {
        for (size_t i = 0; rates[i] > 0; i++) {
            int id = samplerate_id(rates[i]);
            if (id < 0) {
                write_serial_fmt("不支持的采样率 %d", rates[i]);
                continue;
            }
            device->supported_rates |= MASK32(id);
            nseted++;
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            int id = samplerate_id(rates[i]);
            if (id < 0) {
                write_serial_fmt("不支持的采样率 %d", rates[i]);
                continue;
            }
            device->supported_rates |= MASK32(id);
            nseted++;
        }
    }
    return nseted > 0;
}

bool vsound_set_supported_ch(vsound_t device, int16_t ch) {
    if (device == NULL) return false;
    if (ch < 1 || ch > 16) {
        write_serial_fmt("不支持的声道数 %d", ch);
        return false;
    }
    device->supported_chs |= MASK32(ch - 1);
    return true;
}

void vsound_addbuf(vsound_t device, void *buf) {
    if (device == NULL) return;
    memset(buf, 0, device->bufsize);
    queue_enqueue(device->bufs0, buf);
}

int vsound_played(vsound_t snd) {
    if (snd == NULL) return -1;
    void *buf = queue_dequeue(snd->bufs1);
    if (buf == NULL) return -1;
    memset(buf, 0, snd->bufsize);
    queue_enqueue(snd->bufs0, buf);
    return 0;
}
