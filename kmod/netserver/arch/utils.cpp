#include <arch/utils.h>

extern "C" {

static inline LLheader *ll_as_header(void *node)
{
    return static_cast<LLheader *>(node);
}

static inline const LLheader *ll_as_header_const(const void *node)
{
    return static_cast<const LLheader *>(node);
}

void *LinkedListAllocate(void **LLfirstPtr, uint32_t structSize)
{
    if (LLfirstPtr == nullptr || structSize < sizeof(LLheader)) {
        return nullptr;
    }

    LLheader *node = static_cast<LLheader *>(malloc(structSize));
    if (node == nullptr) {
        return nullptr;
    }

    memset(node, 0, structSize);
    node->next = nullptr;

    if (*LLfirstPtr == nullptr) {
        *LLfirstPtr = node;
        return node;
    }

    LLheader *tail = ll_as_header(*LLfirstPtr);
    while (tail->next != nullptr) {
        tail = tail->next;
    }
    tail->next = node;
    return node;
}

bool LinkedListUnregister(void **LLfirstPtr, const void *LLtarget)
{
    if (LLfirstPtr == nullptr || *LLfirstPtr == nullptr || LLtarget == nullptr) {
        return false;
    }

    LLheader *head = ll_as_header(*LLfirstPtr);
    if (head == LLtarget) {
        *LLfirstPtr = head->next;
        return true;
    }

    for (LLheader *current = head; current->next != nullptr; current = current->next) {
        if (current->next == LLtarget) {
            current->next = ll_as_header(current->next)->next;
            return true;
        }
    }

    return false;
}

bool LinkedListRemove(void **LLfirstPtr, void *LLtarget)
{
    if (!LinkedListUnregister(LLfirstPtr, LLtarget)) {
        return false;
    }

    free(LLtarget);
    return true;
}

bool LinkedListDuplicate(void **LLfirstPtrSource, void **LLfirstPtrTarget,
                         uint32_t structSize)
{
    if (LLfirstPtrSource == nullptr || LLfirstPtrTarget == nullptr ||
        structSize < sizeof(LLheader)) {
        return false;
    }

    for (LLheader *current = ll_as_header(*LLfirstPtrSource); current != nullptr;
         current = current->next) {
        LLheader *copy = static_cast<LLheader *>(
            LinkedListAllocate(LLfirstPtrTarget, structSize));
        if (copy == nullptr) {
            return false;
        }

        memcpy(reinterpret_cast<char *>(copy) + sizeof(copy->next),
               reinterpret_cast<const char *>(current) + sizeof(current->next),
               structSize - sizeof(copy->next));
    }

    return true;
}

void LinkedListPushFrontUnsafe(void **LLfirstPtr, void *LLtarget)
{
    if (LLfirstPtr == nullptr || LLtarget == nullptr) {
        return;
    }

    LLheader *target = ll_as_header(LLtarget);
    target->next = ll_as_header(*LLfirstPtr);
    *LLfirstPtr = target;
}

}
