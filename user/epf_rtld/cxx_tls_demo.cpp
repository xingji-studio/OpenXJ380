// SPDX-License-Identifier: Apache-2.0
#include "fixture_runtime.h"
#include "cxx_tls_probe.h"
#include "../rtld/platform.h"

extern "C" void *__dso_handle;
static int output_fd = 1;
static CxxTlsResult main_result;
static uint32_t ready, proceed, failed;

struct LateObject
{
    int value;
    LateObject() : value(epf_cxx_construct(5)) {}
    ~LateObject() { epf_cxx_destroy(5, value); }
};

static int late_value(void)
{
    static thread_local LateObject object;
    return object.value;
}

struct alignas(64) MainObject
{
    unsigned id;
    int value;
    explicit MainObject(unsigned number) : id(number), value(epf_cxx_construct(number)) {}
    ~MainObject()
    {
        epf_cxx_destroy(id, value);
        // Registration during a destructor must run before older callbacks,
        // even when it reuses the slot from which this callback was popped.
        if (id == 4 && late_value() != (int)epf_cxx_state()->seed * 10 + 5) rtld_exit(105);
    }
};

static thread_local MainObject namespace_object(1);

struct Probe
{
    unsigned seed;
    CxxTlsResult result;
    int *addresses[4];
};

static void touch(int **addresses)
{
    addresses[0] = &namespace_object.value;
    epf_cxx_touch_library(&addresses[1], &addresses[2]);
    static thread_local MainObject local_object(4);
    addresses[3] = &local_object.value;
}

static void cooperate(void)
{
    if (rtld_syscall(24, 0, 0, 0, 0, 0, 0)) rtld_exit(106);
}

static int64_t fail(int code)
{
    __atomic_store_n(&failed, (unsigned)code, __ATOMIC_RELEASE);
    return code;
}

static int64_t worker(void *argument)
{
    Probe *probe = static_cast<Probe *>(argument);
    CxxTlsState *state = epf_cxx_state();
    if (state->seed || state->constructors[1]) return fail(107);
    state->seed = probe->seed;
    state->sink = &probe->result;
    touch(probe->addresses);
    for (unsigned i = 0; i < 4; ++i)
    {
        if (*probe->addresses[i] != (int)probe->seed * 10 + (int)i + 1) return fail(108);
        *probe->addresses[i] += 1000;
    }
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proceed, __ATOMIC_ACQUIRE)) cooperate();
    for (unsigned round = 0; round < 8; ++round)
    {
        int *again[4];
        touch(again);
        for (unsigned i = 0; i < 4; ++i)
            if (again[i] != probe->addresses[i] || *again[i] != (int)probe->seed * 10 + (int)i + 1001 ||
                state->constructors[i + 1] != 1 || state->destructors[i + 1]) return fail(109);
        cooperate();
    }
    return probe->seed;
}

static bool valid(const CxxTlsResult &result, unsigned seed)
{
    if (!result.completed || result.seed != seed || result.order != 45321) return false;
    for (unsigned i = 1; i <= 5; ++i)
        if (result.constructors[i] != 1 || result.destructors[i] != 1) return false;
    return true;
}

extern "C" int main(int argc, char **argv, char **envp)
{
    if (!argv || !envp || epf_cxx_dso_identity() == &__dso_handle) return 110;
    CxxTlsState *state = epf_cxx_state();
    if (state->constructors[1] || state->constructors[2]) return 111;
    state->seed = 17;
    state->sink = &main_result;
    int *addresses[4];
    touch(addresses);
    for (unsigned i = 0; i < 4; ++i)
    {
        if (*addresses[i] != 171 + (int)i) return 112;
        *addresses[i] += 1000;
    }
    if (argc > 1)
    {
        long fd = rtld_syscall(2, (long)argv[1], 0x241, 0600, 0, 0, 0);
        if (fd < 0) return 113;
        output_fd = (int)fd;
    }
    for (unsigned round = 0; round < 2; ++round)
    {
        EpfThread threads[4] = {};
        Probe probes[4];
        __atomic_store_n(&ready, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&proceed, 0, __ATOMIC_RELAXED);
        for (unsigned i = 0; i < 4; ++i)
        {
            probes[i].seed = 100 + round * 4 + i;
            // The final destructor fills the result before publishing completed.
            probes[i].result.completed = 0;
            if (epf_thread_create(&threads[i], worker, &probes[i])) return 114;
        }
        while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 4)
        {
            if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) return (int)failed;
            cooperate();
        }
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned slot = 0; slot < 4; ++slot)
            {
                if (probes[i].addresses[slot] == addresses[slot]) return 115;
                for (unsigned j = 0; j < i; ++j)
                    if (probes[i].addresses[slot] == probes[j].addresses[slot]) return 116;
            }
        __atomic_store_n(&proceed, 1, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < 4; ++i)
        {
            int64_t result = -1;
            if (epf_thread_join(&threads[i], &result) || result != probes[i].seed ||
                !valid(probes[i].result, probes[i].seed)) return 117;
        }
    }
    touch(addresses);
    for (unsigned i = 0; i < 4; ++i)
        if (*addresses[i] != 1171 + (int)i || state->constructors[i + 1] != 1 || state->destructors[i + 1])
            return 118;
    rtld_write(output_fd, TLS_PROBE_PREFIX "_CXX_TLS PASS threads=8 lazy=1 dtors=1 reverse=1 reentrant=1 private=1\n");
    return 0;
}

__attribute__((destructor)) static void finalize(void)
{
    if (!valid(main_result, 17)) rtld_exit(119);
    rtld_write(output_fd, TLS_PROBE_PREFIX "_CXX_TLS FINI\n");
    if (output_fd != 1)
    {
        rtld_syscall(74, output_fd, 0, 0, 0, 0, 0);
        rtld_syscall(3, output_fd, 0, 0, 0, 0, 0);
    }
}
