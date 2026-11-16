#pragma once

#include <stdint.h>

struct X64_REGS;

typedef bool (*OpenXJ380SyscallHook)(uint64_t syscall_number, struct X64_REGS *regs);
typedef void (*OpenXJ380ProcessExitHook)(void *process);
// FORK runs before child publication and may touch metadata only. EXEC/ABORT
// run outside scheduler-disabled sections and may release detached resources.
#define OPENXJ380_PROCESS_FORK 1U
#define OPENXJ380_PROCESS_EXEC 2U
#define OPENXJ380_PROCESS_FORK_ABORT 3U
typedef int (*OpenXJ380ProcessImageHook)(uint32_t operation, void *process, void *other);

#ifdef __cplusplus
extern "C" {
#endif

int OpenXJ380Socket_RegisterSyscallHook(OpenXJ380SyscallHook hook);
void OpenXJ380Socket_UnregisterSyscallHook(OpenXJ380SyscallHook hook);
bool OpenXJ380Socket_DispatchSyscall(uint64_t syscall_number, struct X64_REGS *regs);
int OpenXJ380Socket_RegisterProcessExitHook(OpenXJ380ProcessExitHook hook);
void OpenXJ380Socket_UnregisterProcessExitHook(OpenXJ380ProcessExitHook hook);
void OpenXJ380Socket_NotifyProcessExit(void *process);
int OpenXJ380Socket_RegisterProcessImageHook(OpenXJ380ProcessImageHook hook);
void OpenXJ380Socket_UnregisterProcessImageHook(OpenXJ380ProcessImageHook hook);
int OpenXJ380Socket_NotifyProcessImage(uint32_t operation, void *process, void *other);

#ifdef __cplusplus
}
#endif
