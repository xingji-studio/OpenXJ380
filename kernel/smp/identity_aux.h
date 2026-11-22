#pragma once
// SPDX-License-Identifier: Apache-2.0

namespace {

static constexpr uint32_t CPU_IDENTITY_AUX_MSR = 0xc0000103U;
static constexpr uint32_t CPU_IDENTITY_AUX_TAG = 0x584a0000U;
static bool cpu_identity_aux_ready;
static bool cpu_identity_aux_topology_done;
static bool cpu_identity_aux_unavailable;
static size_t cpu_identity_aux_prepared_count;
static uint8_t cpu_identity_aux_prepared[MAX_CPU_NUM];

static uint64_t read_cpu_identity_aux()
{
    uint64_t value;
    asm volatile("rdpid %0" : "=r"(value) :: "memory");
    return value;
}

static void publish_cpu_identity_aux()
{
    // An AP may finish before the BSP has completed its firmware scan. Do
    // not enable RDPID on any CPU until every online CPU supports it and has
    // verified its own register. This also handles heterogeneous feature sets.
    if (!__atomic_load_n(&cpu_identity_aux_topology_done, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&cpu_identity_aux_unavailable, __ATOMIC_ACQUIRE)) return;
    size_t count = __atomic_load_n(&xsi->cpu_count, __ATOMIC_ACQUIRE);
    if (count && __atomic_load_n(&cpu_identity_aux_prepared_count, __ATOMIC_ACQUIRE) == count)
        __atomic_store_n(&cpu_identity_aux_ready, true, __ATOMIC_RELEASE);
}

static void prepare_cpu_identity_aux(PROCESSOR_INFO *cpu)
{
    // Startup-only, on this CPU with IRQs masked. IA32_TSC_AUX belongs to the
    // OS; its tagged array index is independent of sparse ACPI/x2APIC IDs and
    // of user-controlled GSBASE. No TSC clock calibration is involved.
    if (!xsi || !cpu) return;
    size_t index = size_t(cpu - xsi->pcr_inf);
    if (index >= MAX_CPU_NUM || __atomic_load_n(&cpu_identity_aux_prepared[index], __ATOMIC_ACQUIRE)) return;
    uint32_t eax, ebx, ecx, edx;
    asm_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    bool supported = false;
    if (eax >= 7)
    {
        asm_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        supported = (ecx & (1U << 22)) != 0;
    }
    if (!supported)
    {
        __atomic_store_n(&cpu_identity_aux_unavailable, true, __ATOMIC_RELEASE);
        return;
    }
    uint32_t token = CPU_IDENTITY_AUX_TAG + uint32_t(index);
    wrmsr(CPU_IDENTITY_AUX_MSR, token);
    if (read_cpu_identity_aux() != token)
    {
        __atomic_store_n(&cpu_identity_aux_unavailable, true, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&cpu_identity_aux_prepared[index], 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&cpu_identity_aux_prepared_count, 1, __ATOMIC_ACQ_REL);
    publish_cpu_identity_aux();
}

static void finish_cpu_identity_aux_topology()
{
    __atomic_store_n(&cpu_identity_aux_topology_done, true, __ATOMIC_RELEASE);
    publish_cpu_identity_aux();
}

static PROCESSOR_INFO *current_cpu_from_aux()
{
    if (!__atomic_load_n(&cpu_identity_aux_ready, __ATOMIC_ACQUIRE)) return NULL;
    uint64_t token = read_cpu_identity_aux();
    if (token < CPU_IDENTITY_AUX_TAG) return NULL;
    uint64_t index = token - CPU_IDENTITY_AUX_TAG;
    if (index >= MAX_CPU_NUM || index >= __atomic_load_n(&xsi->cpu_count, __ATOMIC_ACQUIRE)) return NULL;
    PROCESSOR_INFO *cpu = &xsi->pcr_inf[index];
    return cpu->scheduler_queue ? cpu : NULL;
}

} // namespace
