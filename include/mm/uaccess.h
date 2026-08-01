#pragma once

#include <krlibc.h>
#include <proto.hpp>
#include <mm/lazyalloc.h>
#include <mm/page.h>

static inline bool fault_in_user_page(page_directory_t *pagedir, uint64_t page)
{
    if (translate_address(pagedir, page) != 0) return true;

    tcb_t task = get_current_task();
    pcb_t process = task != NULL ? task->parent_group : NULL;
    if (process == NULL || process->pagedir != pagedir) return false;
    if (lazy_tryalloc(process, page) != EOK) return false;
    return translate_address(pagedir, page) != 0;
}

static inline bool copy_user_bytes_pagedir(page_directory_t *pagedir, void *dst, const void *src, size_t size,
                                           bool to_user)
{
    if (size == 0) return true;
    if (pagedir == NULL || dst == NULL || src == NULL) return false;

    uint8_t       *d    = (uint8_t *)dst;
    const uint8_t *s    = (const uint8_t *)src;
    uint64_t       uva  = (uint64_t)(to_user ? dst : src);
    size_t         done = 0;

    while (done < size)
    {
        uint64_t cur_uva = uva + done;
        if (!fault_in_user_page(pagedir, cur_uva & PAGE_MASK)) return false;
        uint64_t phys    = translate_address(pagedir, cur_uva);
        if (phys == 0) return false;

        size_t page_left = PAGE_SIZE - (cur_uva & (PAGE_SIZE - 1));
        size_t chunk     = size - done;
        if (chunk > page_left) chunk = page_left;

        void *kaddr = phys_to_virt(phys);
        if (kaddr == NULL) return false;

        if (to_user) memcpy(kaddr, s + done, chunk);
        else memcpy(d + done, kaddr, chunk);

        done += chunk;
    }

    return true;
}

static inline bool user_range_mapped(page_directory_t *pagedir, const void *ptr, size_t size)
{
    if (size == 0) return true;
    if (pagedir == NULL || ptr == NULL) return false;

    uint64_t start = (uint64_t)ptr;
    if (check_user_overflow(start, size)) return false;

    uint64_t end = start + size - 1;
    if (end < start) return false;

    uint64_t page      = start & PAGE_MASK;
    uint64_t last_page = end & PAGE_MASK;
    while (true)
    {
        uint64_t flags = 0;
        if (!fault_in_user_page(pagedir, page)) return false;
        if (!page_table_get_flags(pagedir, page, &flags)) return false;
        if ((flags & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return false;
        if (page == last_page) break;
        if (~page < (uint64_t)PAGE_SIZE - 1) return false;
        page += PAGE_SIZE;
    }

    return true;
}

static inline bool copy_from_user_pagedir(page_directory_t *pagedir, void *dst, const void *src, size_t size)
{
    if (size == 0) return true;
    if (dst == NULL) return false;
    if (!user_range_mapped(pagedir, src, size)) return false;
    return copy_user_bytes_pagedir(pagedir, dst, src, size, false);
}

static inline bool copy_to_user_pagedir(page_directory_t *pagedir, void *dst, const void *src, size_t size)
{
    if (size == 0) return true;
    if (src == NULL) return false;
    if (!user_range_mapped(pagedir, dst, size)) return false;
    return copy_user_bytes_pagedir(pagedir, dst, src, size, true);
}
