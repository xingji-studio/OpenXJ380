// SPDX-License-Identifier: Apache-2.0
// Shared by both interpreter bootstraps and their user runtime libraries. No compiler TLS
// or libc may be used here: this code creates the first thread pointer itself.
#include <stdint.h>
#include <native_runtime.h>
#include "tls.h"
#include "platform.h"

#define PAGE 4096ULL
#define UP(value) (((value) + PAGE - 1) & ~(PAGE - 1))
#define PUBLIC __attribute__((visibility("default")))

static void runtime_error(void) __attribute__((noreturn));

static void runtime_error(void)
{
    rtld_write(2, "xj380 runtime: invalid TLS runtime state\n");
    rtld_exit(127);
}

static XjTlsControl *control(void)
{
    XjTlsControl *result;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(result));
    if (!result || result->self != (uint64_t)result || result->magic != XJ_TLS_CONTROL_MAGIC) runtime_error();
    return result;
}

typedef struct
{
    void (*function)(void *);
    void *object, *dso;
} Destructor;

// Pages form a private LIFO stack owned by the thread pointer, shared by the
// bootstrap and runtime DSO without either image owning a process-global list.
#define DESTRUCTORS_PER_PAGE ((PAGE - 3 * sizeof(uint64_t)) / sizeof(Destructor))
#define MAX_DESTRUCTOR_PAGES 64ULL
typedef struct DestructorPage
{
    struct DestructorPage *previous;
    uint64_t used, pages;
    Destructor entries[DESTRUCTORS_PER_PAGE];
} DestructorPage;
_Static_assert(sizeof(DestructorPage) <= PAGE, "TLS destructor page size");

PUBLIC int __cxa_thread_atexit_impl(void (*function)(void *), void *object, void *dso)
{
    if (!function) return -1;
    XjTlsControl *tcb = control();
    DestructorPage *head = (DestructorPage *)tcb->destructors;
    if (!head || head->used == DESTRUCTORS_PER_PAGE)
    {
        if (head && head->pages == MAX_DESTRUCTOR_PAGES) return -1;
        long mapped = rtld_syscall(9, 0, PAGE, 3, 0x22, -1, 0);
        if (rtld_failed(mapped)) return -1;
        DestructorPage *page = (DestructorPage *)mapped;
        page->previous = head;
        page->pages = head ? head->pages + 1 : 1;
        tcb->destructors = (uint64_t)page;
        head = page;
    }
    head->entries[head->used++] = (Destructor){function, object, dso};
    return 0;
}

PUBLIC int __cxa_thread_atexit(void (*function)(void *), void *object, void *dso)
{
    return __cxa_thread_atexit_impl(function, object, dso);
}

void xj_tls_run_destructors(void)
{
    XjTlsControl *tcb = control();
    while (tcb->destructors)
    {
        DestructorPage *head = (DestructorPage *)tcb->destructors;
        if (head->used)
        {
            // Remove the entry before calling user code. The destructor may
            // initialize another thread_local, including on this same page.
            Destructor entry = head->entries[--head->used];
            entry.function(entry.object);
        }
        else
        {
            tcb->destructors = (uint64_t)head->previous;
            rtld_syscall(11, (long)head, PAGE, 0, 0, 0, 0);
        }
    }
}

uint64_t xj_tls_allocate(const XjTlsContext *context)
{
    if (!context || context->magic != XJ_TLS_CONTEXT_MAGIC || context->version != 1 ||
        !context->module_count || context->module_count > XJ_TLS_MAX_MODULES ||
        context->block_size > XJ_TLS_MAX_TOTAL || context->alignment < 16 ||
        context->alignment > XJ_TLS_MAX_ALIGN || (context->alignment & (context->alignment - 1))) return 0;
    uint64_t size = UP(context->block_size + context->alignment + XJ_TLS_CONTROL_SIZE +
                       (context->module_count + 1) * sizeof(uint64_t));
    long allocation = rtld_syscall(9, 0, (long)size, 3, 0x22, -1, 0);
    if (rtld_failed(allocation)) return 0;
    uint64_t pointer = ((uint64_t)allocation + context->block_size + context->alignment - 1) &
                       ~(context->alignment - 1);
    XjTlsControl *tcb = (XjTlsControl *)pointer;
    uint64_t *dtv = (uint64_t *)(pointer + XJ_TLS_CONTROL_SIZE);
    *tcb = (XjTlsControl){pointer, (uint64_t)dtv, (uint64_t)context, (uint64_t)allocation, size,
                           context->canary, XJ_TLS_CONTROL_MAGIC, 0};
    for (uint64_t i = 0; i < context->module_count; ++i)
    {
        const XjTlsModule *module = &context->modules[i];
        if (module->end > context->block_size || module->memory_size > module->end ||
            module->file_size > module->memory_size || (module->file_size && !module->template_address))
        {
            rtld_syscall(11, allocation, (long)size, 0, 0, 0, 0);
            return 0;
        }
        if (!module->memory_size) continue;
        dtv[i + 1] = pointer - module->end;
        uint8_t *out = (uint8_t *)dtv[i + 1];
        const uint8_t *in = (const uint8_t *)module->template_address;
        for (uint64_t byte = 0; byte < module->file_size; ++byte) out[byte] = in[byte];
        // Anonymous mappings supply the zero-initialized TLS BSS and padding.
    }
    return pointer;
}

int64_t xj_tls_install(uint64_t pointer)
{
    return rtld_syscall(158, 0x1002, (long)pointer, 0, 0, 0, 0);
}

void xj_tls_release(uint64_t pointer)
{
    XjTlsControl *tcb = (XjTlsControl *)pointer;
    if (!tcb || tcb->self != pointer || tcb->magic != XJ_TLS_CONTROL_MAGIC) runtime_error();
    // A worker killed before completing may still have pending callbacks. Its
    // joiner discards their storage; running them with the joiner's FS is wrong.
    while (tcb->destructors)
    {
        DestructorPage *head = (DestructorPage *)tcb->destructors;
        tcb->destructors = (uint64_t)head->previous;
        rtld_syscall(11, (long)head, PAGE, 0, 0, 0, 0);
    }
    rtld_syscall(11, (long)tcb->allocation, (long)tcb->allocation_size, 0, 0, 0, 0);
}

PUBLIC void *__tls_get_addr(const XjTlsIndex *index)
{
    XjTlsControl *tcb = control();
    const XjTlsContext *context = (const XjTlsContext *)tcb->context;
    if (!index) runtime_error();
    if (!index->module && !index->offset) return 0; // Unresolved weak GD reference.
    if (!index->module || index->module > context->module_count ||
        index->offset >= context->modules[index->module - 1].memory_size) runtime_error();
    uint64_t base = ((const uint64_t *)tcb->dtv)[index->module];
    if (!base) runtime_error();
    return (void *)(base + index->offset);
}

typedef struct
{
    uint32_t tid, joining, completed, reserved;
    int64_t result;
    uint64_t tls, stack, stack_size;
    XjThreadEntry entry;
    void *argument;
} ThreadState;

extern long xj_clone_start(uint64_t flags, uint64_t stack, uint32_t *parent_tid,
                            uint32_t *child_tid, uint64_t tls, ThreadState *state);

void xj_thread_entry(ThreadState *state) __attribute__((noreturn, visibility("hidden")));

void xj_thread_entry(ThreadState *state)
{
    int64_t result = state->entry(state->argument);
    xj_tls_run_destructors();
    __atomic_store_n(&state->result, result, __ATOMIC_RELEASE);
    __atomic_store_n(&state->completed, 1, __ATOMIC_RELEASE);
    rtld_syscall(60, 0, 0, 0, 0, 0, 0); // Exit this thread, without process finalizers.
    for (;;) __asm__ volatile("ud2");
}

PUBLIC int64_t xj_thread_create(XjThread *thread, XjThreadEntry entry, void *argument)
{
    if (!thread || !entry || thread->handle) return -22;
    XjTlsControl *current = control();
    long allocated = rtld_syscall(9, 0, PAGE, 3, 0x22, -1, 0);
    if (rtld_failed(allocated)) return allocated;
    ThreadState *state = (ThreadState *)allocated;
    state->entry = entry;
    state->argument = argument;
    state->tls = xj_tls_allocate((const XjTlsContext *)current->context);
    long failure = -12;
    if (!state->tls) goto fail;
    state->stack_size = (1ULL << 20) + 2 * PAGE;
    allocated = rtld_syscall(9, 0, (long)state->stack_size, 0, 0x22, -1, 0);
    if (rtld_failed(allocated)) { failure = allocated; goto fail; }
    state->stack = (uint64_t)allocated;
    failure = rtld_syscall(10, allocated + PAGE, (long)state->stack_size - 2 * PAGE, 3, 0, 0, 0);
    if (failure) goto fail;
    // The child starts with its own FS base and guarded stack. The parent-TID
    // store happens before publication; only quiescent reaping clears it.
    __atomic_store_n(&state->tid, (uint32_t)-1, __ATOMIC_RELAXED);
    failure = xj_clone_start(0x00390f00, state->stack + state->stack_size - PAGE,
                              &state->tid, &state->tid, state->tls, state);
    if (failure < 0) goto fail;
    thread->handle = (uint64_t)state;
    return 0;
fail:
    if (state->stack) rtld_syscall(11, (long)state->stack, (long)state->stack_size, 0, 0, 0, 0);
    if (state->tls) xj_tls_release(state->tls);
    rtld_syscall(11, (long)state, PAGE, 0, 0, 0, 0);
    return failure;
}

PUBLIC int64_t xj_thread_join(XjThread *thread, int64_t *result)
{
    if (!thread || !thread->handle) return -22;
    ThreadState *state = (ThreadState *)thread->handle;
    if (control()->self == state->tls) return -35;
    if (__atomic_exchange_n(&state->joining, 1, __ATOMIC_ACQUIRE)) return -16;
    for (;;)
    {
        uint32_t tid = __atomic_load_n(&state->tid, __ATOMIC_ACQUIRE);
        if (!tid) break;
        long status = rtld_syscall(202, (long)&state->tid, 0, tid, 0, 0, 0);
        if (status && status != -11 && status != -4 && status != -110)
        {
            __atomic_store_n(&state->joining, 0, __ATOMIC_RELEASE);
            return status;
        }
    }
    uint32_t completed = __atomic_load_n(&state->completed, __ATOMIC_ACQUIRE);
    if (result && completed) *result = __atomic_load_n(&state->result, __ATOMIC_ACQUIRE);
    xj_tls_release(state->tls);
    rtld_syscall(11, (long)state->stack, (long)state->stack_size, 0, 0, 0, 0);
    rtld_syscall(11, (long)state, PAGE, 0, 0, 0, 0);
    thread->handle = 0;
    return completed ? 0 : -125;
}
