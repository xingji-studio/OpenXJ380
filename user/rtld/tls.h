// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

// Format-independent x86-64 variant-II TLS ABI. Keep the existing EPF layout.
#define XJ_TLS_MAX_MODULES 32
#define XJ_TLS_MAX_TOTAL (16ULL * 1024 * 1024)
#define XJ_TLS_MAX_ALIGN (2ULL * 1024 * 1024)
#define XJ_TLS_MAX_MODULE (1ULL * 1024 * 1024)

#define XJ_TLS_CONTEXT_MAGIC 0x31534c5446504524ULL
#define XJ_TLS_CONTROL_MAGIC 0x3143545446504524ULL
#define XJ_TLS_CONTROL_SIZE 64ULL

typedef struct
{
    uint64_t end, memory_size, file_size, template_address;
} XjTlsModule;

typedef struct
{
    uint64_t magic, version, module_count, block_size, alignment, canary, reserved[2];
    XjTlsModule modules[XJ_TLS_MAX_MODULES];
} XjTlsContext;

typedef struct
{
    uint64_t self, dtv, context, allocation, allocation_size, canary, magic, destructors;
} XjTlsControl;

#ifdef __cplusplus
static_assert(sizeof(XjTlsControl) == XJ_TLS_CONTROL_SIZE, "XJ380 thread-pointer ABI");
#else
_Static_assert(sizeof(XjTlsControl) == XJ_TLS_CONTROL_SIZE, "XJ380 thread-pointer ABI");
#endif

typedef struct { uint64_t module, offset; } XjTlsIndex;
uint64_t xj_tls_allocate(const XjTlsContext *context);
int64_t xj_tls_install(uint64_t pointer);
void xj_tls_release(uint64_t pointer);
void xj_tls_run_destructors(void);
