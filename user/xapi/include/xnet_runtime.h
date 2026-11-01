// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "stdint.h"
#ifdef __cplusplus
extern "C" {
#endif
// SDK/provider glue, not a syscall ABI. Entropy never falls back to clocks or
// addresses; unavailable checked hardware entropy is an explicit error.
int xapi_secure_random(void *output, uint32_t bytes);
int xapi_tls_runtime_init(void);
struct mbedtls_x509_crt;
int xapi_tls_verify_time(void *clock_error, struct mbedtls_x509_crt *certificate, int depth, uint32_t *flags);
#ifdef __cplusplus
}
#endif
