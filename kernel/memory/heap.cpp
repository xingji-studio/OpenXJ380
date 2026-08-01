#include <mm/alloc/alloc.h>
#include <dlinker.h>
#include <mm/heap.h>

extern "C" void *__real_malloc(size_t size);
extern "C" void *__real_realloc(void *ptr, size_t size);
extern "C" void *__real_aligned_alloc(size_t alignment, size_t size);

static void *retry_heap_alloc(void *(*alloc_fn)(size_t), size_t size)
{
    void *ptr = alloc_fn(size);
    if (ptr != NULL || size == 0 || !kernel_heap_ready()) return ptr;

    if (!kernel_heap_extend(size)) return NULL;
    return alloc_fn(size);
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
    void *new_ptr = __real_realloc(ptr, size);
    if (new_ptr != NULL || size == 0 || !kernel_heap_ready()) return new_ptr;

    if (!kernel_heap_extend(size)) return NULL;
    return __real_realloc(ptr, size);
}

extern "C" void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    void *ptr = __real_aligned_alloc(alignment, size);
    if (ptr != NULL || size == 0 || !kernel_heap_ready()) return ptr;

    size_t extend_size = size + alignment;
    if (extend_size < size) extend_size = size;
    if (!kernel_heap_extend(extend_size)) return NULL;
    return __real_aligned_alloc(alignment, size);
}

void *calloc(size_t num, size_t size)
{
    size_t tot = num * size;
    void  *ptr = malloc(tot);
    if (ptr) memset(ptr, 0, tot);
    return ptr;
}
EXPORT_SYMBOL(calloc);
