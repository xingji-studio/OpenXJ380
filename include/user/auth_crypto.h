// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

// Serialized by the authentication service mutex, never by a spin lock.
void auth_erase(void *data, size_t bytes);
bool auth_equal(const void *a, const void *b, size_t bytes);
int auth_random(void *data, size_t bytes);
int auth_password_hash(const char *password, const uint8_t salt[32], uint32_t iterations, uint8_t hash[32]);
int auth_public_key(uint8_t modulus[384]);
int auth_decrypt(const uint8_t ciphertext[384], void *plaintext, size_t capacity, size_t *bytes);
void auth_forget_abandoned_contexts();
