#pragma once

extern "C"{
#include "../../../include/proto.hpp"
}

typedef struct LLheader LLheader;

struct LLheader {
    LLheader *next;
    // ...
};

#ifdef __cplusplus
extern "C" {
#endif

void *LinkedListAllocate(void **LLfirstPtr, uint32_t structSize);
bool LinkedListUnregister(void **LLfirstPtr, const void *LLtarget);
bool LinkedListRemove(void **LLfirstPtr, void *LLtarget);
bool LinkedListDuplicate(void **LLfirstPtrSource, void **LLfirstPtrTarget,
                         uint32_t structSize);
void LinkedListPushFrontUnsafe(void **LLfirstPtr, void *LLtarget);

#ifdef __cplusplus
}
#endif
