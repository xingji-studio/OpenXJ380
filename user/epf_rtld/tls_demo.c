// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>
#include "fixture_runtime.h"
#include "../rtld/platform.h"

extern int *epf_tls_address(void), *epf_tls_ie_address(void);
extern int epf_tls_check(int initial, int tail);
static __thread int main_data = 7;
static __thread unsigned char main_zero[513] __attribute__((aligned(64)));
static uint32_t ready, proceed, failed;
#ifdef EPF_TLS_TRACE
#define TRACE(message) rtld_syscall(7427, (long)("TLS_TRACE " message "\n"), 0, 0, 0, 0, 0)
#else
#define TRACE(message) ((void)0)
#endif

static int64_t failure(int code)
{
    __atomic_store_n(&failed, (unsigned)code, __ATOMIC_RELEASE);
    return code;
}
static int output_fd = 1;

typedef struct { unsigned index; uint64_t main_address, library_address, ie_address; } Probe;

__attribute__((constructor)) static void initialize(void)
{
    TRACE("constructor");
    if (main_data != 7 || !epf_tls_check(43, 9)) rtld_exit(81);
    main_data = 8;
    main_zero[512] = 11;
}

static int64_t worker(void *argument)
{
    Probe *probe = argument;
    TRACE("worker entered");
    if (main_data != 7 || !epf_tls_check(41, 0) || *epf_tls_ie_address() != 73) return failure(82);
    for (unsigned i = 0; i < sizeof(main_zero); ++i) if (main_zero[i]) return failure(83);
    if ((uintptr_t)main_zero % 64) return failure(84);
    probe->main_address = (uint64_t)&main_data;
    probe->library_address = (uint64_t)epf_tls_address();
    probe->ie_address = (uint64_t)epf_tls_ie_address();
    int value = (int)probe->index + 101;
    main_data = value;
    *epf_tls_address() = value + 10;
    *epf_tls_ie_address() = value + 20;
    main_zero[512] = (unsigned char)value;
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proceed, __ATOMIC_ACQUIRE)) rtld_syscall(24, 0, 0, 0, 0, 0, 0);
    for (unsigned i = 0; i < 8; ++i)
    {
        rtld_syscall(24, 0, 0, 0, 0, 0, 0);
        if (main_data != value || *epf_tls_address() != value + 10 || *epf_tls_ie_address() != value + 20 ||
            main_zero[512] != (unsigned char)value) return failure(85);
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    TRACE("main entered");
    if (!argv || !envp || main_data != 8 || main_zero[512] != 11 ||
        !epf_tls_check(43, 9) || *epf_tls_ie_address() != 73) return 86;
    if (argc > 1)
    {
        long fd = rtld_syscall(2, (long)argv[1], 0x241, 0600, 0, 0, 0);
        if (fd < 0) return 87;
        output_fd = (int)fd;
    }
    for (unsigned round = 0; round < 2; ++round)
    {
        EpfThread threads[4] = {{0}};
        Probe probes[4] = {{0}};
        __atomic_store_n(&ready, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&proceed, 0, __ATOMIC_RELAXED);
        for (unsigned i = 0; i < 4; ++i)
        {
            probes[i].index = i;
            TRACE("create begin");
            if (epf_thread_create(&threads[i], worker, &probes[i])) return 88;
            TRACE("create returned");
        }
        while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 4)
        {
            if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) return (int)failed;
            rtld_syscall(24, 0, 0, 0, 0, 0, 0);
        }
        TRACE("all workers ready");
        for (unsigned i = 0; i < 4; ++i)
        {
            if (probes[i].main_address == (uint64_t)&main_data ||
                probes[i].library_address == (uint64_t)epf_tls_address() ||
                probes[i].ie_address == (uint64_t)epf_tls_ie_address()) return 89;
            for (unsigned j = 0; j < i; ++j)
                if (probes[i].main_address == probes[j].main_address ||
                    probes[i].library_address == probes[j].library_address) return 90;
        }
        __atomic_store_n(&proceed, 1, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < 4; ++i)
        {
            int64_t result = -1;
            TRACE("join begin");
            if (epf_thread_join(&threads[i], &result) || result || threads[i].handle) return 91;
            if (epf_thread_join(&threads[i], &result) != -22) return 92;
            TRACE("join returned");
        }
        if (main_data != 8 || main_zero[512] != 11 || !epf_tls_check(43, 9) ||
            *epf_tls_ie_address() != 73) return 93;
    }
    rtld_write(output_fd, TLS_PROBE_PREFIX "_TLS PASS threads=8 gd=1 ie=1 le=1 init=1 zero=1 isolation=1\n");
    return 0;
}

__attribute__((destructor)) static void finalize(void)
{
    if (main_data != 8 || !epf_tls_check(43, 9)) rtld_exit(94);
    rtld_write(output_fd, TLS_PROBE_PREFIX "_TLS FINI\n");
    if (output_fd != 1)
    {
        rtld_syscall(74, output_fd, 0, 0, 0, 0, 0);
        rtld_syscall(3, output_fd, 0, 0, 0, 0, 0);
    }
}
