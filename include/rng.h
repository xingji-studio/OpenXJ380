#pragma once

#include <stdint.h>

void rng_chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t output[64]);
void get_random_bytes(void *buffer, size_t length);
// Checked hardware-only entropy; failure wipes the entire supplied buffer.
// No timing fallback. Use for keys, authentication and hardware-entropy consumers.
bool get_hardware_random_bytes(void *buffer, size_t length);
// Public identifiers only: hardware preferred, otherwise a software PRNG with
// timing and sequence diversification. This is not cryptographic entropy.
// Failure wipes valid output; used_hardware is optional and false on failure.
bool get_identifier_random_bytes(void *buffer, size_t length, bool *used_hardware = nullptr);
