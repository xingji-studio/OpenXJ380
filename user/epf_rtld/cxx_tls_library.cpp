// SPDX-License-Identifier: Apache-2.0
#include "cxx_tls_probe.h"
#include "../rtld/platform.h"

static __thread CxxTlsState state;
extern "C" void *__dso_handle;

extern "C" CxxTlsState *epf_cxx_state(void)
{
    return &state;
}

extern "C" int epf_cxx_construct(unsigned id)
{
    if (!id || id > 5 || !state.seed || state.constructors[id]++) rtld_exit(101);
    return (int)state.seed * 10 + (int)id;
}

extern "C" void epf_cxx_destroy(unsigned id, int value)
{
    if (!id || id > 5 || state.destructors[id]++ || value != (int)state.seed * 10 + (int)id +
        (id == 5 ? 0 : 1000)) rtld_exit(102);
    state.order = state.order * 10 + id;
    if (id == 1)
    {
        state.completed = 1;
        if (!state.sink) rtld_exit(103);
        state.sink->order = state.order;
        state.sink->seed = state.seed;
        for (unsigned i = 1; i <= 5; ++i)
        {
            state.sink->constructors[i] = state.constructors[i];
            state.sink->destructors[i] = state.destructors[i];
        }
        state.sink->completed = 1;
    }
}

struct alignas(128) LibraryObject
{
    unsigned id;
    int value;
    explicit LibraryObject(unsigned number) : id(number), value(epf_cxx_construct(number)) {}
    ~LibraryObject() { epf_cxx_destroy(id, value); }
};

static thread_local LibraryObject namespace_object(2);

extern "C" void epf_cxx_touch_library(int **first, int **last)
{
    *first = &namespace_object.value;
    static thread_local LibraryObject local_object(3);
    *last = &local_object.value;
}

extern "C" void *epf_cxx_dso_identity(void)
{
    return &__dso_handle;
}

__attribute__((destructor)) static void finalize(void)
{
    if (state.constructors[2] && (!state.completed || state.order != 45321)) rtld_exit(104);
}
