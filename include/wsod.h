#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <cpu/longm.h>
#include <smp/smp.h>

// No allocation, locks, scheduler services or VFS are permitted after entry.
struct WsodCpuState
{
    X64_REGS regs;
    uint64_t cr0, cr2, cr3, cr4, fs_base, gs_base, kernel_gs_base, efer;
    uint64_t lapic, tid, pid, preemption_depth, task_address;
    uint16_t fs, gs, tr;
    char     thread_name[64], process_name[64];
    alignas(16) unsigned char fxsave[512];
    bool     has_regs, has_fxsave;
    unsigned captured;
};

// Fatal entry points; optional product callbacks are declared weak in wsod.cpp.
[[noreturn]] void   do_wsod(const char *error, X64_REGS *regs);
[[noreturn]] void   no_regs_wsod(const char *error);
// Used by the product hook and the standalone fallback. Single-writer entry;
// all other CPUs snapshot and halt. False means recursive/secondary entry.
void                wsod_setup_stacks(tss_t *tss, unsigned index);
bool                wsod_begin(X64_REGS *regs);
bool                wsod_stop_other_cpus();
bool                wsod_nmi_stop(X64_REGS *regs);
const WsodCpuState *wsod_cpu_states();
unsigned            wsod_owner_index();
unsigned            wsod_online_cpus();
bool                wsod_read_memory(uint64_t address, void *destination, unsigned bytes);
[[noreturn]] void   wsod_halt();
