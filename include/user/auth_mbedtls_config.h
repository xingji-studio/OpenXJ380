// SPDX-License-Identifier: Apache-2.0
#pragma once

// Authentication-only kernel build; no TLS, filesystem, PSA, time or host entropy.
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_STD_MEM_HDR <stdlib.h>
#define MBEDTLS_PLATFORM_SNPRINTF_ALT
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO calloc
#define MBEDTLS_PLATFORM_FREE_MACRO free
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_NO_UDBL_DIVISION
#define MBEDTLS_MPI_MAX_SIZE 384
#define MBEDTLS_MPI_WINDOW_SIZE 3

#ifndef UINT64_MAX
#define UINT64_MAX __UINT64_MAX__
#endif
#ifndef UINT32_MAX
#define UINT32_MAX __UINT32_MAX__
#endif
#ifndef SIZE_MAX
#define SIZE_MAX __SIZE_MAX__
#endif
