#pragma once

#include <stdint.h>

struct X64_REGS;

typedef bool (*OpenXJ380SyscallHook)(uint64_t syscall_number, struct X64_REGS *regs);
typedef void (*OpenXJ380ProcessExitHook)(void *process);

#ifdef __cplusplus
extern "C" {
#endif

int OpenXJ380Socket_RegisterSyscallHook(OpenXJ380SyscallHook hook);
void OpenXJ380Socket_UnregisterSyscallHook(OpenXJ380SyscallHook hook);
bool OpenXJ380Socket_DispatchSyscall(uint64_t syscall_number, struct X64_REGS *regs);
int OpenXJ380Socket_RegisterProcessExitHook(OpenXJ380ProcessExitHook hook);
void OpenXJ380Socket_UnregisterProcessExitHook(OpenXJ380ProcessExitHook hook);
void OpenXJ380Socket_NotifyProcessExit(void *process);

#ifdef __cplusplus
}
#endif
