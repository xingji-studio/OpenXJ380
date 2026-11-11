// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../rtld/tls.h"

typedef XjTlsModule EpfTlsModule;
typedef XjTlsContext EpfTlsContext;
typedef XjTlsControl EpfTlsControl;
typedef XjTlsIndex EpfTlsIndex;
#define EPF_TLS_CONTEXT_MAGIC XJ_TLS_CONTEXT_MAGIC
#define EPF_TLS_CONTROL_MAGIC XJ_TLS_CONTROL_MAGIC
#define EPF_TLS_CONTROL_SIZE XJ_TLS_CONTROL_SIZE
#define epf_tls_allocate xj_tls_allocate
#define epf_tls_install xj_tls_install
#define epf_tls_release xj_tls_release
#define epf_tls_run_destructors xj_tls_run_destructors
