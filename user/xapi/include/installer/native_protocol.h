#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original versioned user ABI. No kernel address, device slot, lease or disk
// identity is a user field. All reserved bytes must be zero on input.

typedef __UINT32_TYPE__ xj_inst_native_u32;
typedef __UINT64_TYPE__ xj_inst_native_u64;
typedef __INT64_TYPE__ xj_inst_native_i64;

#define XJ_INST_NATIVE_VERSION 1U
#define XJ_INST_NATIVE_SYSCALL 128956723895689229ULL
#define XJ_INST_NATIVE_MAX_TARGETS 32U
#define XJ_INST_NATIVE_NAME_BYTES 64U
#define XJ_INST_NATIVE_FILESYSTEM_EXT4 2U
#define XJ_INST_NATIVE_MODE_FRESH 0U
#define XJ_INST_NATIVE_LANGUAGE_PACKAGE 0xffffffffU
#define XJ_INST_NATIVE_COMPONENTS_FULL 15ULL

#define XJ_INST_NATIVE_INFO 1U
#define XJ_INST_NATIVE_ENUMERATE 2U
#define XJ_INST_NATIVE_RELEASE_TARGETS 3U
#define XJ_INST_NATIVE_SUBMIT 4U
#define XJ_INST_NATIVE_QUERY 5U
#define XJ_INST_NATIVE_CANCEL 6U
#define XJ_INST_NATIVE_RELEASE_JOB 7U

#define XJ_INST_NATIVE_INFO_SESSION 1ULL
#define XJ_INST_NATIVE_INFO_READY 2ULL
#define XJ_INST_NATIVE_INFO_EXT4 4ULL
#define XJ_INST_NATIVE_TARGET_WRITABLE 1U
#define XJ_INST_NATIVE_TARGET_FLUSH 2U
#define XJ_INST_NATIVE_LIST_CAPACITY_REACHED 1U

#define XJ_INST_NATIVE_JOB_QUEUED 0U
#define XJ_INST_NATIVE_JOB_RUNNING 1U
#define XJ_INST_NATIVE_JOB_SUCCEEDED 2U
#define XJ_INST_NATIVE_JOB_FAILED 3U
#define XJ_INST_NATIVE_JOB_CANCELLED 4U
#define XJ_INST_NATIVE_JOB_QUARANTINED 5U
#define XJ_INST_NATIVE_JOB_OUTCOME_UNKNOWN 1ULL
#define XJ_INST_NATIVE_JOB_IO_STARTED 2ULL
#define XJ_INST_NATIVE_JOB_IO_FAILED 4ULL
#define XJ_INST_NATIVE_JOB_ACTIVATION_ATTEMPTED 8ULL
#define XJ_INST_NATIVE_JOB_ACTIVATION_DURABLE 16ULL

typedef struct xj_inst_native_header
{
    xj_inst_native_u32 version, bytes;
    xj_inst_native_u64 reserved[2];
} xj_inst_native_header;

typedef struct xj_inst_native_info
{
    xj_inst_native_header header;
    xj_inst_native_u64 session_epoch, components, package_bytes, flags;
    xj_inst_native_u32 filesystem, language;
} xj_inst_native_info;

typedef struct xj_inst_native_target
{
    xj_inst_native_u64 handle, bytes;
    xj_inst_native_u32 sector_bytes, flags;
    char name[XJ_INST_NATIVE_NAME_BYTES];
} xj_inst_native_target;

typedef struct xj_inst_native_targets
{
    xj_inst_native_header header;
    xj_inst_native_u64 session_epoch;
    xj_inst_native_u32 count, flags;
    xj_inst_native_target targets[XJ_INST_NATIVE_MAX_TARGETS];
} xj_inst_native_targets;

typedef struct xj_inst_native_release_targets
{
    xj_inst_native_header header;
    xj_inst_native_u32 count, reserved;
    xj_inst_native_u64 handles[XJ_INST_NATIVE_MAX_TARGETS];
} xj_inst_native_release_targets;

typedef struct xj_inst_native_submit
{
    xj_inst_native_header header;
    xj_inst_native_u64 session_epoch, target_handle, components;
    xj_inst_native_u32 filesystem, mode, language, reserved;
} xj_inst_native_submit;

typedef struct xj_inst_native_handle
{
    xj_inst_native_header header;
    xj_inst_native_u64 session_epoch, handle;
} xj_inst_native_handle;

typedef struct xj_inst_native_job
{
    xj_inst_native_header header;
    xj_inst_native_u64 session_epoch, handle;
    xj_inst_native_u32 state, phase;
    xj_inst_native_i64 error;
    xj_inst_native_u64 flags, read_bytes, written_bytes, package_records, package_data_bytes;
} xj_inst_native_job;

static inline void xj_inst_native_initialize(void *object, __SIZE_TYPE__ bytes)
{
    unsigned char *data = (unsigned char *)object;
    for (__SIZE_TYPE__ i = 0; i < bytes; ++i) data[i] = 0;
    xj_inst_native_header *header = (xj_inst_native_header *)object;
    header->version = XJ_INST_NATIVE_VERSION;
    header->bytes = (xj_inst_native_u32)bytes;
}
