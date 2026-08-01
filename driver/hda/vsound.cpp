#include "hda/vsound.h"
#include "hda/hda.h"
#include "proto.hpp"
#include "krlibc.h"
#include "fs/vfs/vfs.h"

#define ALL_IMPLEMENTATION
#include "rbtree-strptr.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

static rbtree_sp_t vsound_list;

static const uint32_t kHdaPlaybackRate     = 48000;
static const uint16_t kHdaPlaybackChannels = 2;

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

typedef struct {
    vfs_node_t node;
    size_t     offset;
} mp3_vfs_stream_t;

typedef struct {
    vsound_t snd;
    uint16_t src_channels;
    uint32_t src_rate;
    uint64_t src_frame_base;
    uint64_t src_pos_fp;
    uint64_t out_frames;
    int16_t *out_buf;
    size_t   out_pos;
    size_t   out_cap;
} pcm_stream_ctx_t;

static const int index_adjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static const int step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static uint16_t read_le16(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int16_t clamp_s16(int32_t sample) {
    if (sample > 32767) return 32767;
    if (sample < -32768) return -32768;
    return (int16_t)sample;
}

static bool parse_wav(const void *buf, size_t size, wav_fmt_t *fmt_out, const uint8_t **data_out,
                      size_t *data_size_out) {
    if (buf == NULL || fmt_out == NULL || data_out == NULL || data_size_out == NULL) return false;
    if (size < 12) return false;

    const uint8_t *bytes = (const uint8_t *)buf;
    if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0) return false;

    bool   has_fmt  = false;
    bool   has_data = false;
    size_t offset   = 12;

    while (offset + 8 <= size) {
        const uint8_t *chunk      = bytes + offset;
        uint32_t       chunk_size = read_le32(chunk + 4);
        size_t         data_off   = offset + 8;
        size_t         next_off   = data_off + chunk_size + (chunk_size & 1);
        if (data_off + chunk_size > size || next_off > size) return false;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) return false;
            fmt_out->audio_format    = read_le16(bytes + data_off + 0);
            fmt_out->channels        = read_le16(bytes + data_off + 2);
            fmt_out->sample_rate     = read_le32(bytes + data_off + 4);
            fmt_out->block_align     = read_le16(bytes + data_off + 12);
            fmt_out->bits_per_sample = read_le16(bytes + data_off + 14);
            has_fmt                  = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            *data_out      = bytes + data_off;
            *data_size_out = chunk_size;
            has_data       = true;
        }

        offset = next_off;
    }

    return has_fmt && has_data;
}

static bool vsound_prepare_file_playback(vsound_t snd, sound_pcmfmt_t fmt, uint16_t channels,
                                         uint32_t rate) {
    if (snd == NULL) return false;
    snd->fmt      = fmt;
    snd->channels = channels;
    snd->rate     = rate;
    snd->volume   = 1;
    if (vsound_open(snd) != 0) {
        write_serial_fmt("vsound open failed: fmt=%d ch=%d rate=%d\n", fmt, channels, rate);
        return false;
    }
    return true;
}

static void vsound_commit_pending_buffer(vsound_t snd) {
    if (snd == NULL || snd->buf == NULL || snd->bufpos == 0) return;
    memset((uint8_t *)snd->buf + snd->bufpos, 0, snd->bufsize - snd->bufpos);
    queue_enqueue(snd->bufs1, snd->buf);
    if (snd->start_dma) snd->start_dma(snd, snd->buf);
    snd->is_dma_ready = true;
    snd->buf          = NULL;
    snd->bufpos       = 0;
}

static void vsound_wait_for_playback(vsound_t snd, uint64_t frames) {
    if (snd == NULL || snd->rate == 0 || frames == 0) return;

    uint64_t playback_ms = (frames * 1000 + snd->rate - 1) / snd->rate;
    uint64_t buffer_ms   = 0;
    if (snd->bytes_per_sample != 0) {
        uint64_t frames_per_buffer = snd->bufsize / snd->bytes_per_sample;
        buffer_ms                  = (frames_per_buffer * 1000 + snd->rate - 1) / snd->rate;
    }

    delay_ms_hp((uint32_t)(playback_ms + buffer_ms + 20));
}

static int16_t wav_sample_to_s16(const wav_fmt_t *fmt, const uint8_t *sample_ptr) {
    if (fmt == NULL || sample_ptr == NULL) return 0;

    switch (fmt->audio_format) {
    case 1:
        switch (fmt->bits_per_sample) {
        case 8: return (int16_t)(((int)sample_ptr[0] - 128) << 8);
        case 16: return (int16_t)read_le16(sample_ptr);
        case 24: {
            int32_t value = (int32_t)(sample_ptr[0] | (sample_ptr[1] << 8) | (sample_ptr[2] << 16));
            if (value & 0x00800000) value |= ~0x00ffffff;
            return (int16_t)(value >> 8);
        }
        case 32: return (int16_t)((int32_t)read_le32(sample_ptr) >> 16);
        default: return 0;
        }
    case 3:
        if (fmt->bits_per_sample == 32) {
            float value = *(const float *)sample_ptr;
            if (value > 1.0f) value = 1.0f;
            if (value < -1.0f) value = -1.0f;
            return clamp_s16((int32_t)(value * 32767.0f));
        }
        return 0;
    default: return 0;
    }
}

static bool play_pcm_s16_compatible(vsound_t snd, const int16_t *src, uint64_t src_frames,
                                    uint16_t src_channels, uint32_t src_rate) {
    if (snd == NULL || src == NULL || src_frames == 0 || src_rate == 0) return false;
    if (src_channels < 1 || src_channels > 2) return false;

    if (!vsound_prepare_file_playback(snd, SOUND_FMT_S16, kHdaPlaybackChannels, kHdaPlaybackRate)) return false;

    static const size_t kFramesPerChunk = 1024;
    int16_t            *pcm             = (int16_t *)malloc(kFramesPerChunk * kHdaPlaybackChannels * sizeof(int16_t));
    if (pcm == NULL) {
        write_serial_fmt("playback conversion alloc failed\n");
        vsound_close(snd);
        return false;
    }

    const uint64_t out_frames = (src_frames * kHdaPlaybackRate + src_rate - 1) / src_rate;
    const uint64_t step_fp    = ((uint64_t)src_rate << 32) / kHdaPlaybackRate;
    uint64_t       src_pos_fp = 0;

    write_serial_fmt("audio playback: %d Hz/%d ch -> %d Hz/%d ch, frames=%d -> %d\n", src_rate, src_channels,
                     kHdaPlaybackRate, kHdaPlaybackChannels, (int)src_frames, (int)out_frames);

    for (uint64_t out_frame = 0; out_frame < out_frames; out_frame += kFramesPerChunk) {
        size_t todo = MIN((uint64_t)kFramesPerChunk, out_frames - out_frame);
        for (size_t i = 0; i < todo; ++i) {
            uint64_t src_frame = src_pos_fp >> 32;
            if (src_frame >= src_frames) src_frame = src_frames - 1;

            int16_t left  = src[src_frame * src_channels];
            int16_t right = src_channels > 1 ? src[src_frame * src_channels + 1] : left;
            pcm[i * 2]    = left;
            pcm[i * 2 + 1] = right;
            src_pos_fp += step_fp;
        }
        vsound_write(snd, (uint64_t *)pcm, todo);
    }

    free(pcm);
    vsound_commit_pending_buffer(snd);
    vsound_wait_for_playback(snd, out_frames);
    vsound_close(snd);
    return true;
}

static size_t mp3_vfs_read(void *user_data, void *buffer_out, size_t bytes_to_read) {
    mp3_vfs_stream_t *stream = (mp3_vfs_stream_t *)user_data;
    if (stream == NULL || stream->node == NULL || buffer_out == NULL || bytes_to_read == 0) return 0;

    size_t got = vfs_read(stream->node, buffer_out, stream->offset, bytes_to_read);
    if (got == (size_t)VFS_STATUS_FAILED) return 0;
    stream->offset += got;
    return got;
}

static drmp3_bool32 mp3_vfs_seek(void *user_data, int offset, drmp3_seek_origin origin) {
    mp3_vfs_stream_t *stream = (mp3_vfs_stream_t *)user_data;
    if (stream == NULL || stream->node == NULL || offset < 0) return DRMP3_FALSE;

    size_t base = origin == drmp3_seek_origin_current ? stream->offset : 0;
    size_t next = base + (size_t)offset;
    if (next < base || next > stream->node->size) return DRMP3_FALSE;
    stream->offset = next;
    return DRMP3_TRUE;
}

static bool pcm_stream_begin(pcm_stream_ctx_t *ctx, vsound_t snd, uint16_t src_channels, uint32_t src_rate,
                             size_t out_frames_per_chunk) {
    if (ctx == NULL || snd == NULL || src_channels < 1 || src_channels > 2 || src_rate == 0 ||
        out_frames_per_chunk == 0)
        return false;

    memset(ctx, 0, sizeof(*ctx));
    ctx->snd          = snd;
    ctx->src_channels = src_channels;
    ctx->src_rate     = src_rate;
    ctx->out_cap      = out_frames_per_chunk;
    ctx->out_buf      = (int16_t *)malloc(out_frames_per_chunk * kHdaPlaybackChannels * sizeof(int16_t));
    if (ctx->out_buf == NULL) return false;

    if (!vsound_prepare_file_playback(snd, SOUND_FMT_S16, kHdaPlaybackChannels, kHdaPlaybackRate)) {
        free(ctx->out_buf);
        ctx->out_buf = NULL;
        return false;
    }

    return true;
}

static bool pcm_stream_flush(pcm_stream_ctx_t *ctx) {
    if (ctx == NULL || ctx->snd == NULL) return false;
    if (ctx->out_pos == 0) return true;
    vsound_write(ctx->snd, (uint64_t *)ctx->out_buf, ctx->out_pos);
    ctx->out_pos = 0;
    return true;
}

static bool pcm_stream_push_s16(pcm_stream_ctx_t *ctx, const int16_t *src, uint64_t src_frames) {
    if (ctx == NULL || src == NULL || src_frames == 0) return true;

    const uint64_t step_fp   = ((uint64_t)ctx->src_rate << 32) / kHdaPlaybackRate;
    const uint64_t chunk_end = ctx->src_frame_base + src_frames;

    while ((ctx->src_pos_fp >> 32) < chunk_end) {
        uint64_t src_frame = ctx->src_pos_fp >> 32;
        size_t   idx       = (size_t)(src_frame - ctx->src_frame_base);

        int16_t left  = src[idx * ctx->src_channels];
        int16_t right = ctx->src_channels > 1 ? src[idx * ctx->src_channels + 1] : left;

        ctx->out_buf[ctx->out_pos * 2]     = left;
        ctx->out_buf[ctx->out_pos * 2 + 1] = right;
        ctx->out_pos++;
        ctx->out_frames++;
        ctx->src_pos_fp += step_fp;

        if (ctx->out_pos == ctx->out_cap) pcm_stream_flush(ctx);
    }

    ctx->src_frame_base = chunk_end;
    return true;
}

static void pcm_stream_end(pcm_stream_ctx_t *ctx) {
    if (ctx == NULL || ctx->snd == NULL) return;
    pcm_stream_flush(ctx);
    vsound_commit_pending_buffer(ctx->snd);
    vsound_wait_for_playback(ctx->snd, ctx->out_frames);
    vsound_close(ctx->snd);
    free(ctx->out_buf);
    ctx->out_buf = NULL;
}

static bool play_wav_compatible(vsound_t snd, const wav_fmt_t *fmt, const uint8_t *src,
                                uint64_t src_frames) {
    if (snd == NULL || fmt == NULL || src == NULL || src_frames == 0 || fmt->sample_rate == 0) return false;
    if (fmt->channels < 1 || fmt->channels > 2) return false;

    if (!vsound_prepare_file_playback(snd, SOUND_FMT_S16, kHdaPlaybackChannels, kHdaPlaybackRate)) return false;

    static const size_t kFramesPerChunk = 1024;
    int16_t            *pcm             = (int16_t *)malloc(kFramesPerChunk * kHdaPlaybackChannels * sizeof(int16_t));
    if (pcm == NULL) {
        write_serial_fmt("wav playback alloc failed\n");
        vsound_close(snd);
        return false;
    }

    const size_t   sample_bytes = fmt->bits_per_sample / 8;
    const uint64_t out_frames   = (src_frames * kHdaPlaybackRate + fmt->sample_rate - 1) / fmt->sample_rate;
    const uint64_t step_fp      = ((uint64_t)fmt->sample_rate << 32) / kHdaPlaybackRate;
    uint64_t       src_pos_fp   = 0;

    write_serial_fmt("wav playback: %d Hz/%d ch -> %d Hz/%d ch, frames=%d -> %d\n", fmt->sample_rate, fmt->channels,
                     kHdaPlaybackRate, kHdaPlaybackChannels, (int)src_frames, (int)out_frames);

    for (uint64_t out_frame = 0; out_frame < out_frames; out_frame += kFramesPerChunk) {
        size_t todo = MIN((uint64_t)kFramesPerChunk, out_frames - out_frame);
        for (size_t i = 0; i < todo; ++i) {
            uint64_t src_frame = src_pos_fp >> 32;
            if (src_frame >= src_frames) src_frame = src_frames - 1;

            const uint8_t *src_frame_ptr = src + src_frame * fmt->block_align;
            int16_t        left          = wav_sample_to_s16(fmt, src_frame_ptr);
            int16_t        right = fmt->channels > 1 ? wav_sample_to_s16(fmt, src_frame_ptr + sample_bytes) : left;
            pcm[i * 2]     = left;
            pcm[i * 2 + 1] = right;
            src_pos_fp += step_fp;
        }
        vsound_write(snd, (uint64_t *)pcm, todo);
    }

    free(pcm);
    vsound_commit_pending_buffer(snd);
    vsound_wait_for_playback(snd, out_frames);
    vsound_close(snd);
    return true;
}

void sound_ima_adpcm_encode(ImaAdpcmCtx *ctx, void *dst, const int16_t *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int delta = src[i] - ctx->prev_sample;
        int sb    = delta < 0 ? 8 : 0;
        delta     = delta < 0 ? -delta : delta;
        int code  = 4 * delta / step_table[ctx->index];
        if (code > 7) code = 7;
        ctx->index += index_adjust[code];
        if (ctx->index < 0) ctx->index = 0;
        if (ctx->index > 88) ctx->index = 88;
        ctx->prev_sample = src[i];
        if (i % 2 == 0) {
            ((uint8_t *)dst)[i / 2] = code | sb;
        } else {
            ((uint8_t *)dst)[i / 2] |= (code | sb) << 4;
        }
    }
}

void sound_ima_adpcm_decode(ImaAdpcmCtx *ctx, int16_t *dst, const void *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int  code  = i % 2 == 0 ? ((uint8_t *)src)[i / 2] & 0x0f : ((uint8_t *)src)[i / 2] >> 4;
        bool sb    = code & 8;
        code      &= 7;
        int delta  = (step_table[ctx->index] * code) / 4 + step_table[ctx->index] / 8;
        delta      = sb ? -delta : delta;
        ctx->prev_sample += delta;
        if (ctx->prev_sample > 32767) ctx->prev_sample = 32767;
        if (ctx->prev_sample < -32768) ctx->prev_sample = -32768;
        dst[i]      = ctx->prev_sample;
        ctx->index += index_adjust[code];
        if (ctx->index < 0) ctx->index = 0;
        if (ctx->index > 88) ctx->index = 88;
    }
}
// 是否为有符号
bool _sound_fmt_issigned[] = {
    // - 8bit / 1byte
    true,
    false,
    // - 16bit / 2byte
    true,
    true,
    false,
    false,
    // - 24bit / 3byte
    true,
    true,
    false,
    false,
    // - 24bit / 4byte(low 3byte)
    true,
    true,
    false,
    false,
    // - 32bit / 4byte
    true,
    true,
    false,
    false,
    // - 64bit / 8byte
    true,
    true,
    false,
    false,
    // - 16bit / 2byte  <- float
    true,
    true,
    // - 32bit / 4byte  <- float
    true,
    true,
    // - 64bit / 8byte  <- float
    true,
    true,
};
// 是否为浮点
bool _sound_fmt_isfloat[] = {
    // - 8bit / 1byte
    false,
    false,
    // - 16bit / 2byte
    false,
    false,
    false,
    false,
    // - 24bit / 3byte
    false,
    false,
    false,
    false,
    // - 24bit / 4byte(low 3byte)
    false,
    false,
    false,
    false,
    // - 32bit / 4byte
    false,
    false,
    false,
    false,
    // - 64bit / 8byte
    false,
    false,
    false,
    false,
    // - 16bit / 2byte  <- float
    true,
    true,
    // - 32bit / 4byte  <- float
    true,
    true,
    // - 64bit / 8byte  <- float
    true,
    true,
};
// 是否为大端序
bool _sound_fmt_isbe[] = {
    // - 8bit / 1byte
    false,
    false,
    // - 16bit / 2byte
    false,
    true,
    false,
    true,
    // - 24bit / 3byte
    false,
    true,
    false,
    true,
    // - 24bit / 4byte(low 3byte)
    false,
    true,
    false,
    true,
    // - 32bit / 4byte
    false,
    true,
    false,
    true,
    // - 64bit / 8byte
    false,
    true,
    false,
    true,
    // - 16bit / 2byte  <- float
    false,
    true,
    // - 32bit / 4byte  <- float
    false,
    true,
    // - 64bit / 8byte  <- float
    false,
    true,
};
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

bool sound_fmt_issigned(sound_pcmfmt_t fmt) {
    if (fmt < 0 || fmt >= SOUND_FMT_CNT) return false;
    return _sound_fmt_issigned[fmt];
}

bool sound_fmt_isfloat(sound_pcmfmt_t fmt) {
    if (fmt < 0 || fmt >= SOUND_FMT_CNT) return false;
    return _sound_fmt_isfloat[fmt];
}

bool sound_fmt_isbe(sound_pcmfmt_t fmt) {
    if (fmt < 0 || fmt >= SOUND_FMT_CNT) return false;
    return _sound_fmt_isbe[fmt];
}

int sound_fmt_bytes(sound_pcmfmt_t fmt) {
    if (fmt < 0 || fmt >= SOUND_FMT_CNT) return -1;
    return _sound_fmt_bytes[fmt];
}

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

static void *getbuffer(vsound_t snd) {
    if (snd->bufpos == snd->bufsize) {
        queue_enqueue(snd->bufs1, snd->buf);
        if (snd->start_dma) snd->start_dma(snd, snd->buf);
        snd->is_dma_ready = true;
        snd->buf          = NULL;
    }
    if (snd->buf == NULL) {
        snd->buf    = queue_dequeue(snd->bufs0);
        snd->bufpos = 0;
    }
    return snd->buf;
}

bool vsound_regist(vsound_t device) {
    if (device == NULL) return false;
    if (device->is_registed || device->is_using) return false;
    if (rbtree_sp_get(vsound_list, device->name)) return false;
    rbtree_sp_insert(vsound_list, device->name, device);
    device->is_registed = true;
    return true;
}

#define MASK8(n)  ((uint8_t)1 << (n))
#define MASK16(n) ((uint16_t)1 << (n))
#define MASK32(n) ((uint32_t)1 << (n))
#define MASK64(n) ((uint64_t)1 << (n))
#define MASK(n)   ((size_t)1 << (n))

bool vsound_set_supported_fmt(vsound_t device, int16_t fmt) {
    if (device == NULL) return false;
    if (fmt >= SOUND_FMT_CNT) {
        write_serial_fmt("不支持的采样格式 %d", fmt);
        return false;
    }
    device->supported_fmts |= MASK32(fmt);
    return true;
}

bool vsound_set_supported_rate(vsound_t device, int32_t rate) {
    if (device == NULL) return false;
    int id = samplerate_id(rate);
    if (id < 0) {
        write_serial_fmt("不支持的采样率 %d", rate);
        return false;
    }
    device->supported_rates |= MASK32(id);
    return true;
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

bool vsound_set_supported_chs(vsound_t device, const int16_t *chs, ssize_t len) {
    if (device == NULL) return false;
    size_t nseted = 0;
    if (len < 0) {
        for (size_t i = 0; chs[i] > 0; i++) {
            if (chs[i] < 1 || chs[i] > 16) {
                write_serial_fmt("不支持的声道数 %d", chs[i]);
                continue;
            }
            device->supported_chs |= MASK32(chs[i] - 1);
            nseted++;
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            if (chs[i] < 1 || chs[i] > 16) {
                write_serial_fmt("不支持的声道数 %d", chs[i]);
                continue;
            }
            device->supported_chs |= MASK32(chs[i] - 1);
            nseted++;
        }
    }
    return nseted > 0;
}

void vsound_addbuf(vsound_t device, void *buf) {
    if (device == NULL) return;
    memset(buf, 0, device->bufsize);
    queue_enqueue(device->bufs0, buf);
}

void vsound_addbufs(vsound_t device, void *const *bufs, ssize_t len) {
    if (device == NULL) return;
    if (len < 0) {
        for (size_t i = 0; bufs[i] != NULL; i++) {
            memset(bufs[i], 0, device->bufsize);
            queue_enqueue(device->bufs0, bufs[i]);
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            memset(bufs[i], 0, device->bufsize);
            queue_enqueue(device->bufs0, bufs[i]);
        }
    }
}

vsound_t vsound_find(const char *name) {
    return (vsound_t)rbtree_sp_get(vsound_list, name);
}

int vsound_played(vsound_t snd) {
    if (snd == NULL) return -1;
    void *buf = queue_dequeue(snd->bufs1);
    if (buf == NULL) return -1;
    memset(buf, 0, snd->bufsize);
    queue_enqueue(snd->bufs0, buf);
    return 0;
}

int vsound_clearbuffer(vsound_t snd) {
    if (snd == NULL) return -1;
    void *buf;
    while ((buf = queue_dequeue(snd->bufs1)) != NULL) {
        memset(buf, 0, snd->bufsize);
        queue_enqueue(snd->bufs0, buf);
    }
    if (snd->buf != NULL) {
        memset(snd->buf, 0, snd->bufsize);
        queue_enqueue(snd->bufs0, snd->buf);
    }
    return 0;
}

int vsound_open(vsound_t snd) { // 打开设备
    if (snd->is_using || snd->is_dma_ready || snd->is_running) return -1;
    if ((snd->supported_fmts & MASK32(snd->fmt)) == 0) return -1;
    int id = samplerate_id(snd->rate);
    if (id < 0) return -1;
    if ((snd->supported_rates & MASK32(id)) == 0) return -1;
    if ((snd->supported_chs & MASK32(snd->channels - 1)) == 0) return -1;
    snd->bytes_per_sample = sound_fmt_bytes((sound_pcmfmt_t)snd->fmt) * snd->channels;
    snd->open(snd);
    snd->is_using = true;
    return 0;
}

int vsound_close(vsound_t snd) { // 关闭设备
    if (snd == NULL) return -1;
    snd->close(snd);
    snd->is_using     = false;
    snd->is_dma_ready = false;
    snd->is_running   = false;
    vsound_clearbuffer(snd);
    return 0;
}

int vsound_play(vsound_t snd) { // 开始播放
    if (snd == NULL) return -1;
    if (snd->is_running) return 1;
    int rets = snd->play ? snd->play(snd) : -1;
    if (rets == 0) snd->is_running = true;
    return rets;
}

int vsound_pause(vsound_t snd) { // 暂停播放
    if (snd == NULL) return -1;
    if (!snd->is_running) return 1;
    int rets = snd->pause ? snd->pause(snd) : -1;
    if (rets == 0) snd->is_running = false;
    return rets;
}

int vsound_drop(vsound_t snd) { // 停止播放并丢弃缓冲
    if (snd == NULL) return -1;
    if (!snd->is_running) return 1;
    int rets = snd->drop ? snd->drop(snd) : -1;
    if (rets == 0) snd->is_running = false;
    return rets;
}

int vsound_drain(vsound_t snd) { // 等待播放完毕后停止播放
    if (snd == NULL) return -1;
    if (!snd->is_running) return 1;
    int rets = snd->drain ? snd->drain(snd) : -1;
    if (rets == 0) snd->is_running = false;
    return rets;
}

int vsound_read(vsound_t snd, uint64_t *data, size_t len) { // 读取 (录音)
    if (snd == NULL) return -1;
    if (snd->is_output) return -1;
    if (snd->is_rwmode) {
        if (snd->read) return snd->read(snd, data, len);
        return -1;
    }

    uint8_t *bytes = (uint8_t *)data;
    void    *buf   = getbuffer(snd);
    size_t   size  = len * snd->bytes_per_sample;
    while (size > 0) {
        while ((buf = getbuffer(snd)) == NULL) {
            if (snd->is_dma_ready && snd->name && streq(snd->name, "hda")) hda_poll();
        }
        size_t nread = MIN(size, snd->bufsize - snd->bufpos);
        memcpy(bytes, (uint8_t *)buf + snd->bufpos, nread);
        snd->bufpos += nread;
        bytes       += nread;
        size        -= nread;
    }
    getbuffer(snd); // 刷新一下，如果读空就触发 DMA
    return len;
}

int vsound_write(vsound_t snd, uint64_t *data, size_t len) { // 写入 (播放)
    if (snd == NULL) return -1;
    if (!snd->is_output) return -1;
    if (snd->is_rwmode) {
        if (snd && snd->write) return snd->write(snd, data, len);
        return -1;
    }

    uint8_t *bytes = (uint8_t *)data;
    void    *buf   = getbuffer(snd);
    size_t   size  = len * snd->bytes_per_sample;
    while (size > 0) {
        while ((buf = getbuffer(snd)) == NULL) {
            if (snd->is_dma_ready && snd->name && streq(snd->name, "hda")) hda_poll();
        }
        size_t nwrite = MIN(size, snd->bufsize - snd->bufpos);
        memcpy((uint8_t *)buf + snd->bufpos, bytes, nwrite);
        snd->bufpos += nwrite;
        bytes       += nwrite;
        size        -= nwrite;
    }
    getbuffer(snd); // 刷新一下，如果写满就触发 DMA
    return len;
}

float vsound_getvol(vsound_t snd) {
    return snd ? snd->volume : -1;
}

int vsound_setvol(vsound_t snd, float vol) {
    if (snd && snd->setvol) {
        snd->volume = vol;
        return snd->setvol(snd, vol);
    }
    return -1;
}

vsound_t snd;

void wav_player(const char *path) {
    vfs_node_t n = vfs_open(path);
    if (n == NULL) {
        write_serial_fmt("wav open failed: %s\n", path);
        return;
    }

    void *buf1 = malloc(n->size);
    if (buf1 == NULL) {
        write_serial_fmt("wav alloc failed: %s\n", path);
        return;
    }
    vfs_read(n, buf1, 0, n->size);

    wav_fmt_t      wav_fmt;
    const uint8_t *wav_data      = NULL;
    size_t         wav_data_size = 0;
    if (!parse_wav(buf1, n->size, &wav_fmt, &wav_data, &wav_data_size)) {
        write_serial_fmt("wav parse failed: %s\n", path);
        free(buf1);
        return;
    }

    if (wav_fmt.channels < 1 || wav_fmt.channels > 2 || wav_fmt.block_align == 0) {
        write_serial_fmt("wav unsupported layout: ch=%d align=%d\n", wav_fmt.channels, wav_fmt.block_align);
        free(buf1);
        return;
    }

    const uint64_t frames = wav_data_size / wav_fmt.block_align;
    snd                   = vsound_find("hda");
    if (snd == NULL) {
        write_serial_fmt("hda device not found for wav playback\n");
        free(buf1);
        return;
    }

    if (wav_fmt.audio_format != 1 && wav_fmt.audio_format != 3) {
        write_serial_fmt("wav unsupported format: %d\n", wav_fmt.audio_format);
        free(buf1);
        return;
    }

    play_wav_compatible(snd, &wav_fmt, wav_data, frames);
    free(buf1);
}

void mp3_player(const char *path) {
    vfs_node_t n = vfs_open(path);
    if (n == NULL) {
        write_serial_fmt("mp3 open failed: %s\n", path);
        return;
    }

    snd = vsound_find("hda");
    if (snd == NULL) {
        write_serial_fmt("hda device not found for mp3 playback\n");
        vfs_close(n);
        return;
    }

    mp3_vfs_stream_t stream = {
        .node   = n,
        .offset = 0,
    };
    drmp3 mp3;
    memset(&mp3, 0, sizeof(mp3));
    if (!drmp3_init(&mp3, mp3_vfs_read, mp3_vfs_seek, &stream, NULL)) {
        write_serial_fmt("mp3 init failed: %s\n", path);
        vfs_close(n);
        return;
    }

    if (mp3.channels < 1 || mp3.channels > 2) {
        write_serial_fmt("mp3 unsupported channels: %d\n", mp3.channels);
        drmp3_uninit(&mp3);
        vfs_close(n);
        return;
    }

    pcm_stream_ctx_t pcm_stream;
    if (!pcm_stream_begin(&pcm_stream, snd, (uint16_t)mp3.channels, mp3.sampleRate, 1024)) {
        write_serial_fmt("mp3 playback setup failed: %s\n", path);
        drmp3_uninit(&mp3);
        vfs_close(n);
        return;
    }

    write_serial_fmt("mp3 playback: %d Hz/%d ch stream decode\n", mp3.sampleRate, mp3.channels);

    const size_t decode_frames = 1152;
    int16_t     *decode_buf    = (int16_t *)malloc(decode_frames * mp3.channels * sizeof(int16_t));
    if (decode_buf == NULL) {
        write_serial_fmt("mp3 decode buffer alloc failed\n");
        pcm_stream_end(&pcm_stream);
        drmp3_uninit(&mp3);
        vfs_close(n);
        return;
    }

    while (true) {
        drmp3_uint64 frames_read = drmp3_read_pcm_frames_s16(&mp3, decode_frames, decode_buf);
        if (frames_read == 0) break;
        pcm_stream_push_s16(&pcm_stream, decode_buf, frames_read);
    }

    free(decode_buf);
    pcm_stream_end(&pcm_stream);
    drmp3_uninit(&mp3);
    vfs_close(n);
}

void hda_test_tone(void *arg) {
    (void)arg;
    delay_s_hp(3);

    vsound_t test_snd = vsound_find("hda");
    if (test_snd == NULL) {
        write_serial_fmt("hda test: device not found\n");
        return;
    }

    test_snd->fmt      = SOUND_FMT_S16;
    test_snd->channels = 2;
    test_snd->rate     = 48000;
    test_snd->volume   = 1;

    if (vsound_open(test_snd) != 0) {
        write_serial_fmt("hda test: open failed\n");
        return;
    }

    write_serial_fmt("hda test: start 440Hz square wave\n");

    static const int kSampleRate      = 48000;
    static const int kFrequency       = 440;
    static const int kFramesPerChunk  = 1024;
    static const int kToneFrames      = kSampleRate * 3 / 10;
    static const int kSilenceFrames   = kSampleRate * 3 / 20;
    static const int kRepeatCount     = 3;
    static const int16_t kAmplitude   = 12000;
    int16_t              pcm[kFramesPerChunk * 2];
    const int            period = kSampleRate / kFrequency;

    uint64_t global_frame = 0;
    for (int repeat = 0; repeat < kRepeatCount; ++repeat) {
        int frames_written = 0;
        while (frames_written < kToneFrames) {
            int frames = MIN(kFramesPerChunk, kToneFrames - frames_written);
            for (int i = 0; i < frames; ++i, ++global_frame) {
                int16_t sample = ((global_frame % period) < (period / 2)) ? kAmplitude : -kAmplitude;
                pcm[i * 2]     = sample;
                pcm[i * 2 + 1] = sample;
            }
            vsound_write(test_snd, (uint64_t *)pcm, frames);
            frames_written += frames;
        }

        frames_written = 0;
        while (frames_written < kSilenceFrames) {
            int frames = MIN(kFramesPerChunk, kSilenceFrames - frames_written);
            memset(pcm, 0, sizeof(int16_t) * frames * 2);
            vsound_write(test_snd, (uint64_t *)pcm, frames);
            frames_written += frames;
        }
    }

    delay_s_hp(2);
    vsound_close(test_snd);
    write_serial_fmt("hda test: finished\n");
}
