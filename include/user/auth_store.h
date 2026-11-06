// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

// Disk v2 contains no process pointers or plaintext credentials. All fields are
// fixed-width little endian (the native x86_64 byte order).
#define AUTH_STORE_MAGIC 0x32554158U
#define AUTH_STORE_COMMIT_MAGIC 0x43414158U
#define AUTH_STORE_COMMITTED 0x58434d54U
#define AUTH_PASSWORD_ITERATIONS 600000U
#define AUTH_MAX_ACCOUNTS 128U

struct AuthStoreHeader
{
    uint32_t magic, version, count, checksum;
    uint64_t generation;
    uint32_t commit, reserved;
};

struct AuthAccountRecord
{
    char name[64];
    uint32_t role, permissions, iterations, reserved;
    uint64_t revision;
    uint8_t salt[32], hash[32];
};

struct AuthCommitRecord
{
    uint32_t magic, version;
    uint64_t generation;
    uint32_t snapshot_checksum, checksum;
};

struct AuthDatabase
{
    AuthStoreHeader header;
    AuthAccountRecord accounts[AUTH_MAX_ACCOUNTS];
};

static_assert(sizeof(AuthStoreHeader) == 32 && sizeof(AuthAccountRecord) == 152, "Account disk ABI");
static_assert(sizeof(AuthCommitRecord) == 24, "Account commit ABI");
