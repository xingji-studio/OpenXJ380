#include <proto.hpp>
#include <mm/lazyalloc.h>
#include <errno.h>


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

errno_t lazy_tryalloc(pcb_t pcb, uint64_t address) {
    mm_virtual_page_t *virt_page = NULL;
    spin_lock(&pcb->virt_queue->lock);
    queue_foreach(pcb->virt_queue, node) {
        mm_virtual_page_t *vpage = (mm_virtual_page_t *)node->data;
        if (address >= vpage->start && address < vpage->start + vpage->count * PAGE_SIZE) {
            virt_page = vpage;
            break;
        }
    }
    spin_unlock(&pcb->virt_queue->lock);

    if (virt_page == NULL) {
        return -1;
    } else {
        size_t   fault_index = (address - virt_page->start) / PAGE_SIZE;
        uint64_t page_addr   = virt_page->start + fault_index * PAGE_SIZE;

        mm_virtual_page_t *left  = NULL;
        mm_virtual_page_t *right = NULL;

        if (fault_index > 0) {
            left = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            if (left == NULL) return -ENOMEM;
            left->start     = virt_page->start;
            left->count     = fault_index;
            left->flags     = virt_page->flags;
            left->pte_flags = virt_page->pte_flags;
        }

        if (fault_index + 1 < virt_page->count) {
            right = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            if (right == NULL) {
                free(left);
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
            return -ENOMEM;
        }
        page_map_to(pcb->pagedir, page_addr, phys, virt_page->pte_flags | PTE_FRAME_ALLOCATED);

        // 直接通过物理地址映射到内核空间来清零页面，不需要切换页目录
        memset((void *)phys_to_virt(phys), 0, PAGE_SIZE);

        if (left != NULL) {
            left->index     = queue_enqueue(pcb->virt_queue, left);
            if (left->index == (size_t)-1) {
                unmap_page(pcb->pagedir, page_addr);
                free(left);
                free(right);
                return -ENOMEM;
            }
        }

        if (right != NULL) {
            right->index     = queue_enqueue(pcb->virt_queue, right);
            if (right->index == (size_t)-1) {
                if (left != NULL) {
                    mm_virtual_page_t *removed = (mm_virtual_page_t *)queue_remove_at(pcb->virt_queue, left->index);
                    free(removed);
                }
                unmap_page(pcb->pagedir, page_addr);
                free(right);
                return -ENOMEM;
            }
        }

        // 移除原始 mm_virtual_page
        queue_remove_at(pcb->virt_queue, virt_page->index);
        free(virt_page);
        return EOK;
    }
}

void unmap_virtual_page(pcb_t process, uint64_t vaddr, size_t length) {
    uint64_t vaddr_end = vaddr + length;
    do {
        mm_virtual_page_t *vpage = NULL;
        spin_lock(&process->virt_queue->lock);
        queue_foreach(process->virt_queue, node) {
            mm_virtual_page_t *virtual_page = (mm_virtual_page_t *)node->data;
            uint64_t           start        = virtual_page->start;
            uint64_t           end          = virtual_page->start + virtual_page->count * PAGE_SIZE;

            if (end > vaddr && start < vaddr_end) {
                vpage = virtual_page;
                break;
            }
        }
        spin_unlock(&process->virt_queue->lock);
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
            right->start             = vaddr_end;
            right->count             = (end - vaddr_end) / PAGE_SIZE;
            right->flags             = vpage->flags;
            right->pte_flags         = vpage->pte_flags;
            right->index             = queue_enqueue(process->virt_queue, right);
            goto free_end;
        }

        if (vaddr > start && vaddr_end >= end) {
            // 覆盖右边，保留左段
            mm_virtual_page_t *left = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            left->start             = start;
            left->count             = (vaddr - start) / PAGE_SIZE;
            left->flags             = vpage->flags;
            left->pte_flags         = vpage->pte_flags;
            left->index             = queue_enqueue(process->virt_queue, left);
            goto free_end;
        }

        if (vaddr > start && vaddr_end < end) {
            // 中间部分覆盖，要拆成两段
            mm_virtual_page_t *left = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            left->start             = start;
            left->count             = (vaddr - start) / PAGE_SIZE;
            left->flags             = vpage->flags;
            left->pte_flags         = vpage->pte_flags;
            left->index             = queue_enqueue(process->virt_queue, left);

            mm_virtual_page_t *right = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
            right->start             = vaddr_end;
            right->count             = (end - vaddr_end) / PAGE_SIZE;
            right->flags             = vpage->flags;
            right->pte_flags         = vpage->pte_flags;
            right->index             = queue_enqueue(process->virt_queue, right);
            goto free_end;
        }
    free_end:
        queue_remove_at(process->virt_queue, vpage->index); //删掉信息
        free(vpage);                                        // 释放信息
        vpage = NULL;
    } while (true);
}

bool lazy_infoalloc(pcb_t process, uint64_t vaddr, size_t length, uint64_t page_flags,
                    uint64_t flags) {
    uint64_t count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (process == NULL || process->virt_queue == NULL || count == 0) return false;

    if (flags & MAP_FIXED) {
        unmap_virtual_page(process, vaddr, length);
        unmap_page_range(get_current_directory(), vaddr, length);
        page_map_range_to_random(get_current_directory(), vaddr, length, page_flags);
        return true;
    } else {
        uint64_t new_start = vaddr;
        uint64_t new_end   = 0;
        if (!lazy_range_end(new_start, count, &new_end)) return false;

        while (true) {
            mm_virtual_page_t candidate;
            memset(&candidate, 0, sizeof(candidate));
            bool found = false;

            spin_lock(&process->virt_queue->lock);
            queue_foreach(process->virt_queue, node) {
                mm_virtual_page_t *vpage = (mm_virtual_page_t *)node->data;
                uint64_t           vpage_end;
                if (vpage == NULL || vpage->flags != flags || vpage->pte_flags != page_flags) continue;
                if (!lazy_range_end(vpage->start, vpage->count, &vpage_end)) continue;
                if (new_end < vpage->start || new_start > vpage_end) continue;

                candidate = *vpage;
                found     = true;
                break;
            }
            spin_unlock(&process->virt_queue->lock);

            if (!found) break;

            mm_virtual_page_t *removed = (mm_virtual_page_t *)queue_remove_at(process->virt_queue, candidate.index);
            if (removed == NULL) continue;

            uint64_t removed_end = 0;
            if (lazy_range_end(removed->start, removed->count, &removed_end)) {
                new_start = MIN(new_start, removed->start);
                new_end   = MAX(new_end, removed_end);
            }
            free(removed);
        }

        mm_virtual_page_t *virt_page = (mm_virtual_page_t*)malloc(sizeof(mm_virtual_page_t));
        if (virt_page == NULL) return false;
        // not_null_assets(virt_page, "Out of memory for virtual page allocation");
        virt_page->start     = new_start;
        virt_page->count     = (new_end - new_start) / PAGE_SIZE;
        virt_page->flags     = flags;
        virt_page->pte_flags = page_flags;
        virt_page->index     = queue_enqueue(process->virt_queue, virt_page);
        if (virt_page->index == (size_t)-1) {
            free(virt_page);
            return false;
        }
        return true;
    }
}

void lazy_free(pcb_t process) {
    if (process == NULL || process->virt_queue == NULL) return;
    if (process->virt_queue->size > 0) {
    refree_virt:
        mm_virtual_page_t *virt_page = NULL;

        queue_foreach(process->virt_queue, node) {
            mm_virtual_page_t *vpage = (mm_virtual_page_t *)node->data;
            virt_page                = vpage;
            break;
        }
        if (virt_page != NULL) {
            queue_remove_at(process->virt_queue, virt_page->index);
            free(virt_page);
            goto refree_virt;
        }
    }
}
