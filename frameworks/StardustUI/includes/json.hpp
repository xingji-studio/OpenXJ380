#include "rapidjson/rapidjson.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "../platforms/platform.hpp"
#ifdef XJ380
#include "../platforms/xj380/xapi/xposix/stdlib.h"
#endif
class JsonAllocator {
public:
    static const bool kNeedFree = true;
    
    void* Malloc(size_t size) { 
        if (size) 
            return malloc(size);  // 直接调用 malloc
        else
            return NULL;
    }
    
    void* Realloc(void* originalPtr, size_t originalSize, size_t newSize) {
        (void)originalSize;
        if (newSize == 0) {
            free(originalPtr);
            return NULL;
        }
        return realloc(originalPtr, newSize);  // 直接调用 realloc
    }
    
    static void Free(void *ptr) { 
        free(ptr);  // 直接调用 free
    }
};
