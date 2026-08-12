#pragma once

#include <stdint.h>

struct X64_REGS;

typedef bool (*OpenXJ380SyscallHook)(uint64_t syscall_number, struct X64_REGS *regs);

#ifdef __cplusplus
extern "C" {
#endif

int OpenXJ380Socket_RegisterSyscallHook(OpenXJ380SyscallHook hook);
void OpenXJ380Socket_UnregisterSyscallHook(OpenXJ380SyscallHook hook);
bool OpenXJ380Socket_DispatchSyscall(uint64_t syscall_number, struct X64_REGS *regs);

#ifdef __cplusplus
}
#endif
