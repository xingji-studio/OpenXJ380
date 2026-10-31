// SPDX-License-Identifier: Apache-2.0
// Original user-side marshaling for the versioned native installer service.
#include "include/installer_native.h"
#include "include/stdint.h"

extern "C" uint64_t enter_syscall(uint64_t number, uint64_t first, uint64_t second, uint64_t third,
                                  uint64_t fourth, uint64_t fifth, uint64_t sixth);

namespace
{
bool valid_header(const xj_inst_native_header &header, __SIZE_TYPE__ bytes)
{
    return header.version == XJ_INST_NATIVE_VERSION && header.bytes == bytes &&
           !header.reserved[0] && !header.reserved[1];
}

xj_inst_native_i64 call(unsigned operation, const void *request, __SIZE_TYPE__ request_bytes,
                         void *output, __SIZE_TYPE__ output_bytes)
{
    return (xj_inst_native_i64)enter_syscall(XJ_INST_NATIVE_SYSCALL, operation, (uint64_t)request,
                                            request_bytes, (uint64_t)output, output_bytes, 0);
}

template <typename T> xj_inst_native_i64 read(unsigned operation, const void *request,
                                             __SIZE_TYPE__ request_bytes, T *output)
{
    if (!output) return -22;
    xj_inst_native_initialize(output, sizeof(*output));
    xj_inst_native_i64 result = call(operation, request, request_bytes, output, sizeof(*output));
    if (result < 0 || !valid_header(output->header, sizeof(*output)))
    {
        xj_inst_native_initialize(output, sizeof(*output));
        return result < 0 ? result : -5;
    }
    return result;
}

xj_inst_native_i64 handle_operation(unsigned operation, const xj_inst_native_handle *handle)
{
    if (!handle || !valid_header(handle->header, sizeof(*handle)) || !handle->session_epoch || !handle->handle)
        return -22;
    return call(operation, handle, sizeof(*handle), 0, 0);
}
} // namespace

extern "C" xj_inst_native_i64 xapi_InstallerNativeInfo(xj_inst_native_info *output)
{
    xj_inst_native_header request;
    xj_inst_native_initialize(&request, sizeof(request));
    return read(XJ_INST_NATIVE_INFO, &request, sizeof(request), output);
}

extern "C" xj_inst_native_i64 xapi_InstallerNativeEnumerate(xj_inst_native_targets *output)
{
    xj_inst_native_header request;
    xj_inst_native_initialize(&request, sizeof(request));
    xj_inst_native_i64 result = read(XJ_INST_NATIVE_ENUMERATE, &request, sizeof(request), output);
    if (result < 0) return result;
    if (!output->session_epoch || output->count > XJ_INST_NATIVE_MAX_TARGETS)
    {
        xj_inst_native_initialize(output, sizeof(*output));
        return -5;
    }
    for (unsigned index = 0; index < output->count; ++index)
    {
        // Names are presentation only; never interpret one as device authority.
        output->targets[index].name[XJ_INST_NATIVE_NAME_BYTES - 1] = 0;
        if (!output->targets[index].handle)
        {
            xj_inst_native_initialize(output, sizeof(*output));
            return -5;
        }
    }
    return 0;
}

extern "C" xj_inst_native_i64 xapi_InstallerNativeReleaseTargets(const xj_inst_native_targets *targets)
{
    if (!targets || targets->count > XJ_INST_NATIVE_MAX_TARGETS) return -22;
    xj_inst_native_release_targets request;
    xj_inst_native_initialize(&request, sizeof(request));
    for (unsigned index = 0; index < targets->count; ++index)
        if (targets->targets[index].handle) request.handles[request.count++] = targets->targets[index].handle;
    if (!request.count) return 0;
    return call(XJ_INST_NATIVE_RELEASE_TARGETS, &request, sizeof(request), 0, 0);
}

extern "C" xj_inst_native_i64 xapi_InstallerNativeSubmit(const xj_inst_native_submit *request,
                                                        xj_inst_native_handle *output)
{
    if (output) xj_inst_native_initialize(output, sizeof(*output));
    if (!request || !output || !valid_header(request->header, sizeof(*request))) return -22;
    xj_inst_native_i64 result = read(XJ_INST_NATIVE_SUBMIT, request, sizeof(*request), output);
    if (result < 0) return result;
    if (!output->handle || output->session_epoch != request->session_epoch)
    {
        xj_inst_native_initialize(output, sizeof(*output));
        return -5;
    }
    return 0;
}

extern "C" xj_inst_native_i64 xapi_InstallerNativeQuery(const xj_inst_native_handle *handle,
                                                       xj_inst_native_job *output)
{
    if (output) xj_inst_native_initialize(output, sizeof(*output));
    if (!handle || !output || !valid_header(handle->header, sizeof(*handle)) ||
        !handle->session_epoch || !handle->handle) return -22;
    xj_inst_native_i64 result = read(XJ_INST_NATIVE_QUERY, handle, sizeof(*handle), output);
    if (result < 0) return result;
    if (output->session_epoch != handle->session_epoch || output->handle != handle->handle ||
        output->state > XJ_INST_NATIVE_JOB_QUARANTINED)
    {
        xj_inst_native_initialize(output, sizeof(*output));
        return -5;
    }
    return 0;
}

extern "C" xj_inst_native_i64 xapi_InstallerNativeCancel(const xj_inst_native_handle *handle)
{
    return handle_operation(XJ_INST_NATIVE_CANCEL, handle);
}

extern "C" xj_inst_native_i64 xapi_InstallerNativeReleaseJob(const xj_inst_native_handle *handle)
{
    return handle_operation(XJ_INST_NATIVE_RELEASE_JOB, handle);
}
