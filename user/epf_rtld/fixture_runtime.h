// SPDX-License-Identifier: Apache-2.0
#pragma once
// Exercise the same compiler-generated TLS cases through both file formats.
#ifdef XJ380_NATIVE_ELF
#include <native_runtime.h>
typedef XjThread EpfThread;
#define epf_thread_create xj_thread_create
#define epf_thread_join xj_thread_join
#define TLS_PROBE_PREFIX "NATIVE"
#else
#include <epf_runtime.h>
#define TLS_PROBE_PREFIX "EPF"
#endif
