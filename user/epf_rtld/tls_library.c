// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>

__thread int epf_tls_data = 41;
__thread unsigned char epf_tls_zero[6001] __attribute__((aligned(64)));
int epf_tls_global = 99;
__thread int *epf_tls_pointer = &epf_tls_global;

__attribute__((constructor)) static void initialize(void)
{
    epf_tls_data = 43;
    epf_tls_zero[6000] = 9;
}

int *epf_tls_address(void)
{
    return &epf_tls_data;
}

int epf_tls_check(int initial, int tail)
{
    if (epf_tls_data != initial || epf_tls_pointer != &epf_tls_global || *epf_tls_pointer != 99) return 0;
    for (unsigned i = 0; i < 6000; ++i) if (epf_tls_zero[i]) return 0;
    return epf_tls_zero[6000] == tail && (uintptr_t)epf_tls_zero % 64 == 0;
}
