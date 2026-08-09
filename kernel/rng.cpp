#include <rng.h>

#include <cpu/lock.h>
#include <krlibc.h>
#include <proto.hpp>
#include <task/pcb.h>

static spin_t rng_lock = SPIN_INIT;
static uint32_t rng_key[8];
static uint32_t rng_nonce[3];
static uint32_t rng_counter;
static size_t rng_bytes_since_reseed;
static bool rng_initialized;

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
                            counter, nonce[0], nonce[1], nonce[2]};
    uint32_t state[16];
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
    memset(state, 0, sizeof(state));
}

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static bool hardware_random64(uint64_t *value)
{
    uint32_t eax, ebx, ecx, edx;
    asm_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 7)
    {
        asm_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        if ((ebx & (1U << 18)) != 0)
        {
            for (unsigned i = 0; i < 10; ++i)
            {
                unsigned char ok;
                __asm__ volatile("rdseed %0; setc %1" : "=r"(*value), "=qm"(ok));
                if (ok) return true;
            }
        }
    }
    asm_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if ((ecx & (1U << 30)) != 0)
    {
        for (unsigned i = 0; i < 10; ++i)
        {
            unsigned char ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok));
            if (ok) return true;
        }
    }
    return false;
}

static void reseed_locked(void)
{
    uint64_t seed = nanoTime() ^ (uint64_t)(uintptr_t)&seed ^ (uint64_t)(uintptr_t)get_current_task();
    for (unsigned i = 0; i < 8; ++i)
    {
        uint64_t entropy;
        if (hardware_random64(&entropy)) seed ^= entropy;
        uint64_t mixed = splitmix64(&seed);
        rng_key[i] ^= (uint32_t)mixed ^ (uint32_t)(mixed >> 32);
    }
    uint64_t nonce = splitmix64(&seed);
    rng_nonce[0] ^= (uint32_t)nonce;
    rng_nonce[1] ^= (uint32_t)(nonce >> 32);
    rng_nonce[2] ^= (uint32_t)splitmix64(&seed);
    rng_counter = 1;
    rng_bytes_since_reseed = 0;
    rng_initialized = true;
}

void get_random_bytes(void *buffer, size_t length)
{
    if (buffer == NULL || length == 0) return;
    uint8_t *out = (uint8_t *)buffer;
    spin_lock(&rng_lock);
    if (!rng_initialized) reseed_locked();
    while (length != 0)
    {
        if (rng_counter == 0 || rng_bytes_since_reseed >= 1024 * 1024) reseed_locked();
        uint8_t block[64];
        rng_chacha20_block(rng_key, rng_counter++, rng_nonce, block);
        size_t chunk = length < sizeof(block) ? length : sizeof(block);
        memcpy(out, block, chunk);
        out += chunk;
        length -= chunk;
        rng_bytes_since_reseed += chunk;
        if (chunk < sizeof(block))
        {
            for (unsigned i = 0; i < 8; ++i)
                rng_key[i] ^= (uint32_t)block[32 + i * 4] | ((uint32_t)block[33 + i * 4] << 8) |
                              ((uint32_t)block[34 + i * 4] << 16) | ((uint32_t)block[35 + i * 4] << 24);
        }
        memset(block, 0, sizeof(block));
    }
    spin_unlock(&rng_lock);
}
