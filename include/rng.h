// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

enum class RandomSource : uint8_t { Unavailable, Rdseed, Virtio, Rdrand, Software };

void rng_init();
const char *rng_source_name(RandomSource source);
RandomSource rng_secure_source();
void rng_chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t output[64]);
// Ordinary bytes prefer the secure generator, with a separate software fallback.
void get_random_bytes(void *buffer, size_t length);
// Raw CPU entropy only; retained for callers explicitly requiring CPU hardware.
bool get_hardware_random_bytes(void *buffer, size_t length);
// RDSEED -> virtio-rng -> RDRAND seeds an independent ChaCha20 generator.
// Failure wipes output. Nonblocking requests do not wait for device completion.
bool get_secure_random_bytes(void *buffer, size_t length, RandomSource *source = nullptr, bool nonblocking = false);
// Public identifiers may use diversified software output when no secure source
// exists. used_hardware retains its CPU-only meaning; source identifies virtio.
bool get_identifier_random_bytes(void *buffer, size_t length, bool *used_hardware = nullptr,
                                 RandomSource *source = nullptr);
extern "C" uint32_t kernel_random_u32();
