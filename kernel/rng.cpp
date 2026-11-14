// SPDX-License-Identifier: Apache-2.0
#include <rng.h>

#include <cpu/lock.h>
#include <dlinker.h>
#include <krlibc.h>
#include <proto.hpp>
#include <rtc.h>
#include <task/pcb.h>
#include <virtio/rng.h>

static void secure_zero(void *buffer, size_t bytes)
{
    volatile uint8_t *p = static_cast<volatile uint8_t *>(buffer);
    while (bytes--) *p++ = 0;
}

static inline uint32_t rotl32(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32 - count));
}

static inline void quarter_round(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d)
{
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

void rng_chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t output[64])
{
    static const uint32_t constants[4] = {0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U};
    uint32_t initial[16] = {constants[0], constants[1], constants[2], constants[3],
                            key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                            counter, nonce[0], nonce[1], nonce[2]};  //初始矩阵
    uint32_t state[16];  //state从initial复制出来一份,避免破坏原始数据
    memcpy(state, initial, sizeof(state));
    for (unsigned i = 0; i < 10; ++i)
    {
        quarter_round(state[0], state[4], state[8], state[12]);
        quarter_round(state[1], state[5], state[9], state[13]);
        quarter_round(state[2], state[6], state[10], state[14]);
        quarter_round(state[3], state[7], state[11], state[15]);
        quarter_round(state[0], state[5], state[10], state[15]);
        quarter_round(state[1], state[6], state[11], state[12]);
        quarter_round(state[2], state[7], state[8], state[13]);
        quarter_round(state[3], state[4], state[9], state[14]);
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        uint32_t word = state[i] + initial[i];
        output[i * 4] = (uint8_t)word;
        output[i * 4 + 1] = (uint8_t)(word >> 8);
        output[i * 4 + 2] = (uint8_t)(word >> 16);
        output[i * 4 + 3] = (uint8_t)(word >> 24);
    }
    /* 清理现场,避免它以明文形式残留在栈帧被后续代码读到  */
    secure_zero(state, sizeof(state));
    secure_zero(initial, sizeof(initial));
}

namespace
{
struct Generator
{
    uint32_t key[8], nonce[3], counter;
    size_t bytes;
    bool initialized;
    RandomSource source;
};

spin_t rng_lock = SPIN_INIT;
Generator secure_rng, ordinary_rng;
uint64_t identifier_sequence;
constexpr size_t kReseedBytes = 1024 * 1024;

// Optional instructions are only called after checking the current CPU.
static bool try_rdseed64(uint64_t *value)
{
    unsigned char ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(*value), "=qm"(ok)::"cc");
    return ok != 0;
}

static bool try_rdrand64(uint64_t *value)
{
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok)::"cc");
    return ok != 0;
}

static bool cpu_random(void *buffer, size_t length, bool seed)
{
    uint32_t a, b, c, d;
    asm_cpuid(0, 0, &a, &b, &c, &d);
    if (seed)
    {
        if (a < 7) return false;
        asm_cpuid(7, 0, &a, &b, &c, &d);
        if (!(b & (1U << 18))) return false;
    }
    else
    {
        if (a < 1) return false;
        asm_cpuid(1, 0, &a, &b, &c, &d);
        if (!(c & (1U << 30))) return false;
    }
    size_t offset = 0;
    while (offset < length)
    {
        uint64_t word = 0;
        bool ok = false;
        for (unsigned attempt = 0; attempt < 10 && !ok; ++attempt)
            ok = seed ? try_rdseed64(&word) : try_rdrand64(&word);
        if (!ok)
        {
            secure_zero(&word, sizeof(word));
            secure_zero(buffer, length);
            return false;
        }
        size_t count = length - offset < sizeof(word) ? length - offset : sizeof(word);
        memcpy(static_cast<uint8_t *>(buffer) + offset, &word, count);
        secure_zero(&word, sizeof(word));
        offset += count;
    }
    return true;
}

static bool seed_material(uint8_t seed[44], RandomSource *source, bool nonblocking)
{
    *source = RandomSource::Unavailable;
    if (cpu_random(seed, 44, true)) *source = RandomSource::Rdseed;
    else if (virtio_rng_read(seed, 44, nonblocking)) *source = RandomSource::Virtio;
    else if (cpu_random(seed, 44, false)) *source = RandomSource::Rdrand;
    else
    {
        secure_zero(seed, 44);
        return false;
    }
    return true;
}

static void install_seed(Generator &state, const uint8_t seed[44], RandomSource source)
{
    // Mix into the previous key, then rekey with a private block. Neither a
    // reseed nor the output path ever publishes bytes used as the next key.
    for (unsigned i = 0; i < 8; ++i)
    {
        uint32_t word;
        memcpy(&word, seed + 4 * i, sizeof(word));
        state.key[i] ^= word;
    }
    for (unsigned i = 0; i < 3; ++i)
    {
        uint32_t word;
        memcpy(&word, seed + 32 + 4 * i, sizeof(word));
        state.nonce[i] ^= word;
    }
    uint8_t block[64];
    rng_chacha20_block(state.key, state.counter, state.nonce, block);
    memcpy(state.key, block, sizeof(state.key));
    memcpy(state.nonce, block + 32, sizeof(state.nonce));
    secure_zero(block, sizeof(block));
    state.counter = 1;
    state.bytes = 0;
    state.source = source;
    state.initialized = true;
}

static bool prepare_secure(bool nonblocking)
{
    spin_lock(&rng_lock);
    bool initialized = secure_rng.initialized;
    bool reseed = !initialized || secure_rng.bytes >= kReseedBytes;
    // Reserve the next attempt before dropping the lock. Temporary source
    // failure does not discard a securely seeded generator or cause per-call
    // device waits. Concurrent first users may independently collect seeds.
    if (initialized && reseed) secure_rng.bytes = 0;
    spin_unlock(&rng_lock);
    if (!reseed) return true;
    uint8_t seed[44];
    RandomSource source;
    bool collected = seed_material(seed, &source, nonblocking);
    spin_lock(&rng_lock);
    bool first_seed = collected && !secure_rng.initialized;
    if (collected) install_seed(secure_rng, seed, source);
    initialized = secure_rng.initialized;
    spin_unlock(&rng_lock);
    secure_zero(seed, sizeof(seed));
    if (first_seed) write_serial_fmt("rng: secure source=%s\n", rng_source_name(source));
    return initialized;
}

static void generate_locked(Generator &state, uint8_t *output, size_t count)
{
    uint8_t block[64];
    rng_chacha20_block(state.key, state.counter, state.nonce, block);
    memcpy(output, block, count); // count <= 32; upper half is never disclosed.
    memcpy(state.key, block + 32, sizeof(state.key));
    // A new key is installed at every block, including counter rollover.
    state.counter = state.counter == __UINT32_MAX__ ? 1 : state.counter + 1;
    state.bytes += count;
    secure_zero(block, sizeof(block));
}

static uint64_t read_identifier_tsc()
{
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return (uint64_t(high) << 32) | low;
}

static uint64_t identifier_timestamp()
{
    uint32_t a, b, c, d;
    asm_cpuid(0, 0, &a, &b, &c, &d);
    if (a < 1) return 0;
    asm_cpuid(1, 0, &a, &b, &c, &d);
    return (d & (1U << 4)) ? read_identifier_tsc() : 0;
}

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static bool next_identifier(uint64_t *value)
{
    uint64_t sequence = __atomic_load_n(&identifier_sequence, __ATOMIC_RELAXED);
    do
    {
        if (sequence == __UINT64_MAX__) return false;
    } while (!__atomic_compare_exchange_n(&identifier_sequence, &sequence, sequence + 1, false,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    *value = sequence + 1;
    return true;
}
}

const char *rng_source_name(RandomSource source)
{
    switch (source)
    {
    case RandomSource::Rdseed: return "rdseed";
    case RandomSource::Virtio: return "virtio-rng";
    case RandomSource::Rdrand: return "rdrand";
    case RandomSource::Software: return "software";
    default: return "unavailable";
    }
}

void rng_init()
{
    virtio_rng_init();
    // Boot has scheduling/IRQs closed. Post DMA now, without waiting for it;
    // later runtime callers can collect a completion or wait with scheduling open.
    prepare_secure(true);
    uint8_t discarded;
    get_random_bytes(&discarded, 1);
    secure_zero(&discarded, sizeof(discarded));
    write_serial_fmt("rng: secure source=%s\n", rng_source_name(rng_secure_source()));
}

RandomSource rng_secure_source()
{
    spin_lock(&rng_lock);
    RandomSource source = secure_rng.initialized ? secure_rng.source : RandomSource::Unavailable;
    spin_unlock(&rng_lock);
    return source;
}

bool get_hardware_random_bytes(void *buffer, size_t length)
{
    if (!buffer || !length) return false;
    if (cpu_random(buffer, length, true) || cpu_random(buffer, length, false)) return true;
    secure_zero(buffer, length);
    return false;
}

bool get_secure_random_bytes(void *buffer, size_t length, RandomSource *source, bool nonblocking)
{
    if (source) *source = RandomSource::Unavailable;
    if (!length) return true;
    if (!buffer) return false;
    if (!prepare_secure(nonblocking))
    {
        secure_zero(buffer, length);
        return false;
    }
    auto *output = static_cast<uint8_t *>(buffer);
    while (length)
    {
        size_t count = length < 32 ? length : 32;
        spin_lock(&rng_lock);
        generate_locked(secure_rng, output, count);
        bool reseed = secure_rng.bytes >= kReseedBytes;
        if (source) *source = secure_rng.source;
        spin_unlock(&rng_lock);
        output += count;
        length -= count;
        if (length && reseed) prepare_secure(nonblocking);
    }
    return true;
}

void get_random_bytes(void *buffer, size_t length)
{
    if (!buffer || !length) return;
    // Ordinary randomness may explicitly degrade; it must never initialize or
    // contaminate the independent secure generator with clock-derived material.
    if (get_secure_random_bytes(buffer, length, nullptr, true)) return;
    uint64_t seed_value = realtime_ns() ^ nanoTime() ^ identifier_timestamp() ^
                          uint64_t(uintptr_t(get_current_task()));
    uint8_t seed[44];
    for (size_t offset = 0; offset < sizeof(seed);)
    {
        uint64_t word = splitmix64(&seed_value);
        size_t count = sizeof(seed) - offset < sizeof(word) ? sizeof(seed) - offset : sizeof(word);
        memcpy(seed + offset, &word, count);
        offset += count;
    }
    spin_lock(&rng_lock);
    if (!ordinary_rng.initialized) install_seed(ordinary_rng, seed, RandomSource::Software);
    spin_unlock(&rng_lock);
    secure_zero(seed, sizeof(seed));
    auto *output = static_cast<uint8_t *>(buffer);
    while (length)
    {
        size_t count = length < 32 ? length : 32;
        spin_lock(&rng_lock);
        generate_locked(ordinary_rng, output, count);
        spin_unlock(&rng_lock);
        output += count;
        length -= count;
    }
}

bool get_identifier_random_bytes(void *buffer, size_t length, bool *used_hardware, RandomSource *source)
{
    if (used_hardware) *used_hardware = false;
    if (source) *source = RandomSource::Unavailable;
    if (!buffer || !length) return false;
    RandomSource selected;
    if (get_secure_random_bytes(buffer, length, &selected))
    {
        if (used_hardware) *used_hardware = selected == RandomSource::Rdseed || selected == RandomSource::Rdrand;
        if (source) *source = selected;
        return true;
    }
    uint64_t sequence;
    if (!next_identifier(&sequence))
    {
        secure_zero(buffer, length);
        return false;
    }
    uint32_t key[8];
    get_random_bytes(key, sizeof(key));
    uint64_t timing[3] = {realtime_ns(), nanoTime(), identifier_timestamp()};
    for (unsigned i = 0; i < 3; ++i)
    {
        key[2 * i] ^= uint32_t(timing[i]);
        key[2 * i + 1] ^= uint32_t(timing[i] >> 32);
    }
    uint32_t nonce[3] = {uint32_t(sequence), uint32_t(sequence >> 32), 0x584a4944U};
    uint8_t block[64];
    size_t offset = 0;
    uint64_t counter = 0;
    bool complete = true;
    while (offset < length)
    {
        if (counter > __UINT32_MAX__) { complete = false; break; }
        rng_chacha20_block(key, uint32_t(counter++), nonce, block);
        size_t count = length - offset < sizeof(block) ? length - offset : sizeof(block);
        memcpy(static_cast<uint8_t *>(buffer) + offset, block, count);
        offset += count;
    }
    secure_zero(key, sizeof(key));
    secure_zero(timing, sizeof(timing));
    secure_zero(block, sizeof(block));
    if (!complete) secure_zero(buffer, length);
    else if (source) *source = RandomSource::Software;
    return complete;
}

// lwIP callbacks run under its own protocol locks. This fast adapter only
// consumes the already seeded generator; device polling is never done here.
extern "C" uint32_t kernel_random_u32()
{
    uint32_t value;
    spin_lock(&rng_lock);
    bool ready = secure_rng.initialized || ordinary_rng.initialized;
    if (ready) generate_locked(secure_rng.initialized ? secure_rng : ordinary_rng,
                               reinterpret_cast<uint8_t *>(&value), sizeof(value));
    spin_unlock(&rng_lock);
    if (!ready) get_random_bytes(&value, sizeof(value));
    return value;
}
EXPORT_SYMBOL(kernel_random_u32);
