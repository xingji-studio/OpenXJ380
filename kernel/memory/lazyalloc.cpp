#include <proto.hpp>
#include <mm/lazyalloc.h>
#include <errno.h>

static spin_t lazy_owner_transaction_lock = SPIN_INIT;

class lazy_owner_transaction_guard
{
public:
    lazy_owner_transaction_guard()
    {
        spin_lock(&lazy_owner_transaction_lock);
    }

    ~lazy_owner_transaction_guard()
    {
        spin_unlock(&lazy_owner_transaction_lock);
    }
};

void *virt_copy(void *ptr) {
    if (ptr == NULL) return NULL;
    mm_virtual_page_t *src_page = (mm_virtual_page_t *)ptr;
    mm_virtual_page_t *new_page = (mm_virtual_page_t *)malloc(sizeof(mm_virtual_page_t));
    new_page->start             = src_page->start;
    new_page->flags             = src_page->flags;
    new_page->count             = src_page->count;
    new_page->pte_flags         = src_page->pte_flags;
    new_page->index             = src_page->index;
    return new_page;
}

static bool lazy_range_end(uint64_t start, uint64_t count, uint64_t *end)
{
    if (end == NULL) return false;
    if (count > __UINT64_MAX__ / PAGE_SIZE) return false;
    uint64_t length = count * PAGE_SIZE;
    if (start > __UINT64_MAX__ - length) return false;
    *end = start + length;
    return true;
}

errno_t lazy_tryalloc_owner(const lazy_address_space_owner_t *owner, uint64_t address) {
    if (owner == NULL || owner->pagedir == NULL || owner->virt_queue == NULL) return -EINVAL;
    lazy_owner_transaction_guard guard;
    mm_virtual_page_t *virt_page = NULL;
    spin_lock(&owner->virt_queue->lock);
    queue_foreach(owner->virt_queue, node) {
        mm_virtual_page_t *vpage = (mm_virtual_page_t *)node->data;
        if (address >= vpage->start && address < vpage->start + vpage->count * PAGE_SIZE) {
            virt_page = vpage;
            break;
        }
    }
    if (virt_page == NULL) {
        spin_unlock(&owner->virt_queue->lock);
        return -1;
    }

    /* The found virt_page must remain valid while we mutate the queue, so we
     * hold the queue lock across the entire split/allocate/free sequence. */
    size_t   fault_index = (address - virt_page->start) / PAGE_SIZE;
    uint64_t page_addr   = virt_page->start + fault_index * PAGE_SIZE;
    size_t   virt_page_index = virt_page->index;

    mm_virtual_page_t *left  = NULL;
    mm_virtual_page_t *right = NULL;

    if (fault_index > 0) {
        left = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
        if (left == NULL) {
            spin_unlock(&owner->virt_queue->lock);
            return -ENOMEM;
        }
        left->start     = virt_page->start;
        left->count     = fault_index;
        left->flags     = virt_page->flags;
        left->pte_flags = virt_page->pte_flags;
    }

    if (fault_index + 1 < virt_page->count) {
        right = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
        if (right == NULL) {
            free(left);
            spin_unlock(&owner->virt_queue->lock);
            return -ENOMEM;
        }
        right->start     = virt_page->start + (fault_index + 1) * PAGE_SIZE;
        right->count     = virt_page->count - fault_index - 1;
        right->flags     = virt_page->flags;
        right->pte_flags = virt_page->pte_flags;
    }

    uint64_t phys = alloc_frames(1);
    if (phys == 0) {
        free(left);
        free(right);
        spin_unlock(&owner->virt_queue->lock);
        return -ENOMEM;
    }
    page_map_to(owner->pagedir, page_addr, phys, virt_page->pte_flags | PTE_FRAME_ALLOCATED);
    memset((void *)phys_to_virt(phys), 0, PAGE_SIZE);

    if (left != NULL) {
        left->index = queue_enqueue_locked(owner->virt_queue, left);
        if (left->index == (size_t)-1) {
            unmap_page(owner->pagedir, page_addr);
            free(left);
            free(right);
            spin_unlock(&owner->virt_queue->lock);
            return -ENOMEM;
        }
    }

    if (right != NULL) {
        right->index = queue_enqueue_locked(owner->virt_queue, right);
        if (right->index == (size_t)-1) {
            if (left != NULL) {
                mm_virtual_page_t *removed =
                    (mm_virtual_page_t *)queue_remove_at_locked(owner->virt_queue, left->index);
                free(removed);
            }
            unmap_page(owner->pagedir, page_addr);
            free(right);
            spin_unlock(&owner->virt_queue->lock);
            return -ENOMEM;
        }
    }

    queue_remove_at_locked(owner->virt_queue, virt_page_index);
    spin_unlock(&owner->virt_queue->lock);
    free(virt_page);
    return EOK;
}

errno_t lazy_tryalloc(pcb_t pcb, uint64_t address)
{
    lazy_address_space_owner_t owner = {pcb != NULL ? pcb->pagedir : NULL, pcb != NULL ? pcb->virt_queue : NULL};
    return lazy_tryalloc_owner(&owner, address);
}

static void unmap_virtual_page_owner_locked(const lazy_address_space_owner_t *owner, uint64_t vaddr, size_t length) {
    if (owner == NULL || owner->virt_queue == NULL || length == 0) return;
    uint64_t vaddr_end = vaddr + length;
    spin_lock(&owner->virt_queue->lock);
    do {
        mm_virtual_page_t *vpage = NULL;
        queue_foreach(owner->virt_queue, node) {
            mm_virtual_page_t *virtual_page = (mm_virtual_page_t *)node->data;
            uint64_t           start        = virtual_page->start;
            uint64_t           end          = virtual_page->start + virtual_page->count * PAGE_SIZE;

            if (end > vaddr && start < vaddr_end) {
                vpage = virtual_page;
                break;
            }
        }
        if (vpage == NULL) break;
        uint64_t start = vpage->start;
        uint64_t end   = vpage->start + vpage->count * PAGE_SIZE;

        if (vaddr <= start && vaddr_end >= end) {
            // 完全覆盖
            goto free_end;
        }
        if (vaddr <= start && vaddr_end < end) {
            // 覆盖左边，保留右段
            mm_virtual_page_t *right = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            if (right == NULL) break;
            right->start             = vaddr_end;
            right->count             = (end - vaddr_end) / PAGE_SIZE;
            right->flags             = vpage->flags;
            right->pte_flags         = vpage->pte_flags;
            right->index             = queue_enqueue_locked(owner->virt_queue, right);
            if (right->index == (size_t)-1) {
                free(right);
                break;
            }
            goto free_end;
        }

        if (vaddr > start && vaddr_end >= end) {
            // 覆盖右边，保留左段
            mm_virtual_page_t *left = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            if (left == NULL) break;
            left->start             = start;
            left->count             = (vaddr - start) / PAGE_SIZE;
            left->flags             = vpage->flags;
            left->pte_flags         = vpage->pte_flags;
            left->index             = queue_enqueue_locked(owner->virt_queue, left);
            if (left->index == (size_t)-1) {
                free(left);
                break;
            }
            goto free_end;
        }

        if (vaddr > start && vaddr_end < end) {
            // 中间部分覆盖，要拆成两段
            mm_virtual_page_t *left = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            if (left == NULL) break;
            left->start             = start;
            left->count             = (vaddr - start) / PAGE_SIZE;
            left->flags             = vpage->flags;
            left->pte_flags         = vpage->pte_flags;
            left->index             = queue_enqueue_locked(owner->virt_queue, left);
            if (left->index == (size_t)-1) {
                free(left);
                break;
            }

            mm_virtual_page_t *right = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            if (right == NULL) {
                mm_virtual_page_t *removed =
                    (mm_virtual_page_t *)queue_remove_at_locked(owner->virt_queue, left->index);
                free(removed);
                break;
            }
            right->start             = vaddr_end;
            right->count             = (end - vaddr_end) / PAGE_SIZE;
            right->flags             = vpage->flags;
            right->pte_flags         = vpage->pte_flags;
            right->index             = queue_enqueue_locked(owner->virt_queue, right);
            if (right->index == (size_t)-1) {
                mm_virtual_page_t *removed_right =
                    (mm_virtual_page_t *)queue_remove_at_locked(owner->virt_queue, right->index);
                free(removed_right);
                mm_virtual_page_t *removed_left =
                    (mm_virtual_page_t *)queue_remove_at_locked(owner->virt_queue, left->index);
                free(removed_left);
                free(right);
                break;
            }
            goto free_end;
        }
    free_end:
        queue_remove_at_locked(owner->virt_queue, vpage->index); //删掉信息
        free(vpage);                                        // 释放信息
        vpage = NULL;
    } while (true);
    spin_unlock(&owner->virt_queue->lock);
}

void unmap_virtual_page_owner(const lazy_address_space_owner_t *owner, uint64_t vaddr, size_t length)
{
    lazy_owner_transaction_guard guard;
    unmap_virtual_page_owner_locked(owner, vaddr, length);
}

void unmap_virtual_page(pcb_t process, uint64_t vaddr, size_t length)
{
    lazy_address_space_owner_t owner = {process != NULL ? process->pagedir : NULL,
                                        process != NULL ? process->virt_queue : NULL};
    unmap_virtual_page_owner(&owner, vaddr, length);
}

bool lazy_infoalloc_owner(const lazy_address_space_owner_t *owner, uint64_t vaddr, size_t length,
                          uint64_t page_flags, uint64_t flags) {
    uint64_t count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (owner == NULL || owner->pagedir == NULL || owner->virt_queue == NULL || count == 0) return false;
    lazy_owner_transaction_guard guard;

    if (flags & MAP_FIXED) {
        unmap_virtual_page_owner_locked(owner, vaddr, length);
        unmap_page_range(owner->pagedir, vaddr, length);
        /* Use the checked variant: a mid-range allocation failure rolls
         * back the partial mapping instead of leaving holes the caller
         * would treat as success. */
        return page_map_range_to_random_checked(owner->pagedir, vaddr, length, page_flags);
    } else {
        uint64_t new_start = vaddr;
        uint64_t new_end   = 0;
        if (!lazy_range_end(new_start, count, &new_end)) return false;

        spin_lock(&owner->virt_queue->lock);
        while (true) {
            mm_virtual_page_t candidate;
            memset(&candidate, 0, sizeof(candidate));
            bool found = false;

            queue_foreach(owner->virt_queue, node) {
                mm_virtual_page_t *vpage = (mm_virtual_page_t *)node->data;
                uint64_t           vpage_end;
                if (vpage == NULL || vpage->flags != flags || vpage->pte_flags != page_flags) continue;
                if (!lazy_range_end(vpage->start, vpage->count, &vpage_end)) continue;
                if (new_end < vpage->start || new_start > vpage_end) continue;

                candidate = *vpage;
                found     = true;
                break;
            }

            if (!found) break;

            mm_virtual_page_t *removed = (mm_virtual_page_t *)queue_remove_at_locked(owner->virt_queue, candidate.index);
            if (removed == NULL) break;

            uint64_t removed_end = 0;
            if (lazy_range_end(removed->start, removed->count, &removed_end)) {
                new_start = MIN(new_start, removed->start);
                new_end   = MAX(new_end, removed_end);
            }
            free(removed);
        }

        mm_virtual_page_t *virt_page = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
        if (virt_page == NULL) {
            spin_unlock(&owner->virt_queue->lock);
            return false;
        }
        // not_null_assets(virt_page, "Out of memory for virtual page allocation");
        virt_page->start     = new_start;
        virt_page->count     = (new_end - new_start) / PAGE_SIZE;
        virt_page->flags     = flags;
        virt_page->pte_flags = page_flags;
        virt_page->index     = queue_enqueue_locked(owner->virt_queue, virt_page);
        if (virt_page->index == (size_t)-1) {
            free(virt_page);
            spin_unlock(&owner->virt_queue->lock);
            return false;
        }
        spin_unlock(&owner->virt_queue->lock);
        return true;
    }
}

bool lazy_infoalloc(pcb_t process, uint64_t vaddr, size_t length, uint64_t page_flags, uint64_t flags)
{
    lazy_address_space_owner_t owner = {process != NULL ? process->pagedir : NULL,
                                        process != NULL ? process->virt_queue : NULL};
    return lazy_infoalloc_owner(&owner, vaddr, length, page_flags, flags);
}

void lazy_free_owner(const lazy_address_space_owner_t *owner) {
    if (owner == NULL || owner->virt_queue == NULL) return;
    lazy_owner_transaction_guard guard;
    if (owner->virt_queue->size > 0) {
    refree_virt:
        mm_virtual_page_t *virt_page = NULL;

        queue_foreach(owner->virt_queue, node) {
            mm_virtual_page_t *vpage = (mm_virtual_page_t *)node->data;
            virt_page                = vpage;
            break;
        }
        if (virt_page != NULL) {
            queue_remove_at(owner->virt_queue, virt_page->index);
            free(virt_page);
            goto refree_virt;
        }
    }
}

void lazy_free(pcb_t process)
{
    lazy_address_space_owner_t owner = {process != NULL ? process->pagedir : NULL,
                                        process != NULL ? process->virt_queue : NULL};
    lazy_free_owner(&owner);
}
