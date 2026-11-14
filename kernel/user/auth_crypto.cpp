// SPDX-License-Identifier: Apache-2.0
#include <user/auth_crypto.h>
#include <errno.h>
#include <rng.h>
#include <krlibc.h>
#include <mbedtls/hmac_drbg.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/rsa.h>

namespace
{
mbedtls_hmac_drbg_context random_context;
mbedtls_rsa_context private_key;
bool random_ready, key_ready, entropy_failed;

int entropy(void *, unsigned char *output, size_t bytes)
{
    if (get_secure_random_bytes(output, bytes)) return 0;
    entropy_failed = true;
    return MBEDTLS_ERR_HMAC_DRBG_ENTROPY_SOURCE_FAILED;
}

int prepare_random()
{
    if (random_ready) return 0;
    entropy_failed = false;
    mbedtls_hmac_drbg_init(&random_context);
    const char context[] = "XJ380 authentication v1";
    int error = mbedtls_hmac_drbg_seed(&random_context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                      entropy, NULL, (const unsigned char *)context, sizeof(context));
    if (error)
    {
        mbedtls_hmac_drbg_free(&random_context);
        return entropy_failed ? -ENODATA : -EIO;
    }
    random_ready = true;
    return 0;
}
}

void auth_erase(void *data, size_t bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)data;
    while (bytes--) *cursor++ = 0;
}

bool auth_equal(const void *a, const void *b, size_t bytes)
{
    const uint8_t *left = (const uint8_t *)a, *right = (const uint8_t *)b;
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < bytes; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

int auth_random(void *data, size_t bytes)
{
    int error = prepare_random();
    if (!error && mbedtls_hmac_drbg_random(&random_context, (unsigned char *)data, bytes)) error = -EIO;
    if (error) auth_erase(data, bytes);
    return error;
}

int auth_password_hash(const char *password, const uint8_t salt[32], uint32_t iterations, uint8_t hash[32])
{
    int error = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password,
                                            strlen(password), salt, 32, iterations, 32, hash);
    if (error) auth_erase(hash, 32);
    return error ? -EIO : 0;
}

int auth_public_key(uint8_t modulus[384])
{
    int error = prepare_random();
    if (error) return error;
    if (!key_ready)
    {
        mbedtls_rsa_init(&private_key);
        if (mbedtls_rsa_set_padding(&private_key, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256) ||
            mbedtls_rsa_gen_key(&private_key, mbedtls_hmac_drbg_random, &random_context, 3072, 65537))
        {
            mbedtls_rsa_free(&private_key);
            return entropy_failed ? -ENODATA : -EIO;
        }
        key_ready = true;
    }
    return mbedtls_rsa_export_raw(&private_key, modulus, 384, NULL, 0, NULL, 0, NULL, 0, NULL, 0) ? -EIO : 0;
}

int auth_decrypt(const uint8_t ciphertext[384], void *plaintext, size_t capacity, size_t *bytes)
{
    static const unsigned char label[] = "XJ380 auth v1";
    if (!key_ready) return -EACCES;
    int error = mbedtls_rsa_rsaes_oaep_decrypt(&private_key, mbedtls_hmac_drbg_random, &random_context,
                                             label, sizeof(label) - 1, bytes, ciphertext,
                                             (unsigned char *)plaintext, capacity);
    if (error) auth_erase(plaintext, capacity);
    return error ? -EACCES : 0;
}

void auth_forget_abandoned_contexts()
{
    // A forcibly stopped call may have been between pointer publication and
    // freeing an old MPI buffer. Do not traverse those potentially partial
    // objects. Drop their references and reseed/rekey on the next request.
    // Abnormal termination can leak those allocations, but cannot reuse a
    // damaged private key or cause a double free during service recovery.
    auth_erase(&private_key, sizeof(private_key));
    auth_erase(&random_context, sizeof(random_context));
    key_ready = random_ready = false;
}
