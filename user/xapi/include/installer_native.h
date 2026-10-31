#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <installer/native_protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call these from one stable user thread. Native handles belong to the issuing
// TCB/TID/PID; a window notification callback must marshal requests to that thread.
xj_inst_native_i64 xapi_InstallerNativeInfo(xj_inst_native_info *output);
xj_inst_native_i64 xapi_InstallerNativeEnumerate(xj_inst_native_targets *output);
xj_inst_native_i64 xapi_InstallerNativeReleaseTargets(const xj_inst_native_targets *targets);
xj_inst_native_i64 xapi_InstallerNativeSubmit(const xj_inst_native_submit *request, xj_inst_native_handle *output);
xj_inst_native_i64 xapi_InstallerNativeQuery(const xj_inst_native_handle *handle, xj_inst_native_job *output);
xj_inst_native_i64 xapi_InstallerNativeCancel(const xj_inst_native_handle *handle);
xj_inst_native_i64 xapi_InstallerNativeReleaseJob(const xj_inst_native_handle *handle);

#ifdef __cplusplus
}
#endif
