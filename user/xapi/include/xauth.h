// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "stdint.h"

// Fixed-width user/kernel ABI. Keep the product and XAPI copies identical.
#define XAUTH_VERSION 1U
#define XAPI_AUTH_BEGIN 7506
#define XAPI_AUTH_FINISH 7507
#define XAPI_ACCOUNT_INFO 7508
#define XAUTH_LOGIN 1U
#define XAUTH_CREATE_FIRST 2U
#define XAUTH_VERIFY_PASSWORD 3U
#define XAUTH_CHANGE_PASSWORD 4U
#define XAUTH_PASSWORD_BYTES 128U
#define XAUTH_RSA_BYTES 384U
#define XAUTH_TOKEN_BYTES 32U

typedef struct
{
    uint32_t size, version, operation, reserved;
    char username[64];
    uint8_t grant[XAUTH_TOKEN_BYTES];
} XAuthBegin;

typedef struct
{
    uint32_t size, version, operation, reserved;
    uint64_t id;
    uint8_t nonce[32], modulus[XAUTH_RSA_BYTES];
    uint32_t exponent, reserved2;
    char username[64];
} XAuthChallenge;

// This entire message is OAEP encrypted before entering the kernel.
typedef struct
{
    uint32_t version, operation;
    uint64_t id;
    uint8_t nonce[32];
    char username[64], password[XAUTH_PASSWORD_BYTES];
    uint8_t grant[XAUTH_TOKEN_BYTES];
} XAuthPlaintext;

typedef struct
{
    uint32_t size, version;
    uint64_t id;
    uint8_t ciphertext[XAUTH_RSA_BYTES];
} XAuthRequest;

typedef struct
{
    uint32_t size, version;
    uint8_t grant[XAUTH_TOKEN_BYTES];
} XAuthResult;

typedef struct
{
    uint32_t size, version;
    char username[64];
    uint32_t user_type, permissions;
    uint64_t credential_revision;
} XAccountInfo;

#ifdef __cplusplus
static_assert(sizeof(XAuthBegin) == 112 && sizeof(XAuthChallenge) == 512, "Auth challenge ABI");
static_assert(sizeof(XAuthPlaintext) == 272 && sizeof(XAuthRequest) == 400, "Auth request ABI");
static_assert(sizeof(XAuthResult) == 40 && sizeof(XAccountInfo) == 88, "Auth result ABI");
extern "C" {
#endif
int64_t xapi_GetAccountInfo(XAccountInfo *info);
int64_t xapi_UserVerifyPassword(const char *password, uint8_t grant[XAUTH_TOKEN_BYTES]);
int64_t xapi_UserChangePassword(const char *password, const uint8_t grant[XAUTH_TOKEN_BYTES]);
#ifdef __cplusplus
}
#endif
