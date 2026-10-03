#include <mm/alloc/alloc.h>
#include <dlinker.h>
#include <mm/heap.h>

extern "C" void *__real_malloc(size_t size);
extern "C" void __real_free(void *ptr);
extern "C" void *__real_realloc(void *ptr, size_t size);
extern "C" void *__real_aligned_alloc(size_t alignment, size_t size);

// liballoc's internal spinlock does not mask interrupts. Its owner must not
// be preempted by another allocator user on the same CPU, including IRQ code.
// This guard also works before SMP and the scheduler have been initialized.
static uint64_t allocator_irq_save()
{
    uint64_t flags;
    __asm__ volatile("pushfq\n\t"
                     "pop %0\n\t"
                     "cli\n\t"
                     : "=r"(flags)
                     :
                     : "memory");
    return flags;
}

static void allocator_irq_restore(uint64_t flags)
{
    __asm__ volatile("push %0\n\t"
                     "popfq\n\t"
                     :
                     : "r"(flags)
                     : "memory");
}

static void *retry_heap_alloc(void *(*alloc_fn)(size_t), size_t size)
{
    uint64_t flags = allocator_irq_save();
    void *ptr = alloc_fn(size);
    if (ptr != NULL || size == 0 || !kernel_heap_ready())
    {
        allocator_irq_restore(flags);
        return ptr;
    }

    if (!kernel_heap_extend(size))
    {
        allocator_irq_restore(flags);
        return NULL;
    }
    ptr = alloc_fn(size);
    allocator_irq_restore(flags);
    return ptr;
}

extern "C" void *__wrap_malloc(size_t size)
{
    return retry_heap_alloc(__real_malloc, size);
}

extern "C" void *__wrap_calloc(size_t num, size_t size)
{
    if (size != 0 && num > ((size_t)-1) / size) return NULL;

    size_t tot = num * size;
    void  *ptr = __wrap_malloc(tot);
    if (ptr != NULL) memset(ptr, 0, tot);
    return ptr;
}

extern "C" void *__wrap_realloc(void *ptr, size_t size)
{
    uint64_t flags = allocator_irq_save();
    void *new_ptr = __real_realloc(ptr, size);
    if (new_ptr != NULL || size == 0 || !kernel_heap_ready())
    {
        allocator_irq_restore(flags);
        return new_ptr;
    }

    if (!kernel_heap_extend(size))
    {
        allocator_irq_restore(flags);
        return NULL;
    }
    new_ptr = __real_realloc(ptr, size);
    allocator_irq_restore(flags);
    return new_ptr;
}

extern "C" void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    uint64_t flags = allocator_irq_save();
    void *ptr = __real_aligned_alloc(alignment, size);
    if (ptr != NULL || size == 0 || !kernel_heap_ready())
    {
        allocator_irq_restore(flags);
        return ptr;
    }

    size_t extend_size = size + alignment;
    if (extend_size < size) extend_size = size;
    if (!kernel_heap_extend(extend_size))
    {
        allocator_irq_restore(flags);
        return NULL;
    }
    ptr = __real_aligned_alloc(alignment, size);
    allocator_irq_restore(flags);
    return ptr;
}

extern "C" void __wrap_free(void *ptr)
{
    uint64_t flags = allocator_irq_save();
    __real_free(ptr);
    allocator_irq_restore(flags);
}

void *calloc(size_t num, size_t size)
{
    size_t tot = num * size;
    void  *ptr = malloc(tot);
    if (ptr) memset(ptr, 0, tot);
    return ptr;
}
EXPORT_SYMBOL(calloc);
