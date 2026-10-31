#pragma once

#include <stdint.h>

void rng_chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t output[64]);
void get_random_bytes(void *buffer, size_t length);
// Checked hardware-only entropy; failure wipes the entire supplied buffer.
// No timing fallback. Intended for identities that must not reuse weak seeds.
bool get_hardware_random_bytes(void *buffer, size_t length);
