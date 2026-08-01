#include "../xapi/include/x3api.h"

extern "C" void __real_free(void *ptr);

extern "C" void *g_browser_heap_base;
extern "C" unsigned long long g_browser_heap_size;

namespace
{

static void log_invalid_free(void *ptr)
{
    char buffer[160];
    snprintf(buffer,
             sizeof(buffer),
             "browser: ignored invalid free ptr=%p heap=[%p,%p)\n",
             ptr,
             g_browser_heap_base,
             (void *)((unsigned char *)g_browser_heap_base + g_browser_heap_size));
    xapi_OutputSerial(buffer);
}

static bool pointer_in_browser_heap(void *ptr)
{
    if (ptr == nullptr || g_browser_heap_base == nullptr || g_browser_heap_size == 0) {
        return true;
    }

    const unsigned long long value = (unsigned long long)ptr;
    const unsigned long long begin = (unsigned long long)g_browser_heap_base;
    const unsigned long long end = begin + g_browser_heap_size;
    return value >= begin && value < end;
}

} // namespace

extern "C" void __wrap_free(void *ptr)
{
    if (ptr == nullptr) {
        __real_free(ptr);
        return;
    }

    if (!pointer_in_browser_heap(ptr)) {
        log_invalid_free(ptr);
    }

    __real_free(ptr);
}
