#include <cpu/lock.h>
#include <cpu/fsgsbase.h>
#include <cpu/longm.h>
#include <cpu/regio.h>
#include <dlinker.h>
#include <krlibc.h>
#include <mm/lazyalloc.h>
#include <mm/page.h>
#include <mm/uaccess.h>
#include <proto.hpp>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <stdint.h>

spin_t page_lock;

page_directory_t  kernel_page_dir;
page_directory_t *current_directory = NULL;

extern bool     no_interrupt;
extern uint64_t memory_size;
uint64_t        double_fault_page = 0;
page_t         *page_maps;
void           *early_alloc(size_t size);
extern lock_queue *pcb_group_queue;

static uint64_t page_entry_value(page_directory_t *directory, uint64_t addr);

static inline uint64_t page_align_up(uint64_t length)
{
    if (length == 0) return 0;
    return (length + PAGE_SIZE - 1) & PAGE_MASK;
}

static bool try_map_user_stack_fault(tcb_t task, uint64_t address)
{
    if (task == NULL || task->parent_group == NULL || task->parent_group->pagedir == NULL) return false;
    if (task->user_stack == 0 || task->user_stack_top <= task->user_stack) return false;
    if (address >= task->user_stack_top) return false;

    uint64_t page_addr = address & PAGE_MASK;
    if (page_entry_value(task->parent_group->pagedir, page_addr) & PTE_PRESENT) return false;

    // Allow stack to grow below user_stack, but prevent collision with heap
    if (page_addr < task->user_stack)
    {
        pcb_t pcb = task->parent_group;
        if (pcb->brk_current != 0 && page_addr < pcb->brk_current + PAGE_SIZE) return false;
    }

    uint64_t phys = alloc_frames(1);
    if (phys == 0) return false;
    memset((void *)phys_to_virt(phys), 0, PAGE_SIZE);
    page_map_to(task->parent_group->pagedir, page_addr, phys,
                PTE_PRESENT | PTE_WRITEABLE | PTE_USER | PTE_FRAME_ALLOCATED);

    if (page_addr < task->user_stack) task->user_stack = page_addr;
    return true;
}

static pcb_t find_process_by_pagedir(page_directory_t *directory)
{
    if (directory == NULL || pcb_group_queue == NULL) return NULL;

    pcb_t ret = NULL;
    spin_lock(&pcb_group_queue->lock);
    queue_foreach(pcb_group_queue, node)
    {
        pcb_t pcb = (pcb_t)node->data;
        if (pcb != NULL && pcb->pagedir == directory)
        {
            ret = pcb;
            break;
        }
    }
    spin_unlock(&pcb_group_queue->lock);
    return ret;
}

static uint64_t find_free_user_range(page_directory_t *directory, uint64_t start, uint64_t length)
{
    if (directory == NULL || length == 0) return 0;

    pcb_t owner = find_process_by_pagedir(directory);
    uint64_t aligned_length = page_align_up(length);
    if (aligned_length == 0 || aligned_length >= USER_BRK_START) return 0;

    uint64_t candidate = start < USER_MMAP_START ? USER_MMAP_START : (start & PAGE_MASK);
    const uint64_t limit = USER_BRK_START - aligned_length;

    while (candidate <= limit)
    {
        bool occupied = false;
        if (owner != NULL && vma_find_intersection(&owner->vma_manager, candidate, candidate + aligned_length))
        {
            candidate += PAGE_SIZE;
            continue;
        }
        if (owner != NULL && owner->virt_queue != NULL)
        {
            spin_lock(&owner->virt_queue->lock);
            queue_foreach(owner->virt_queue, node)
            {
                mm_virtual_page_t *vpage = (mm_virtual_page_t *)node->data;
                if (vpage == NULL) continue;

                uint64_t vpage_end = vpage->start + vpage->count * PAGE_SIZE;
                if (!(candidate + aligned_length <= vpage->start || candidate >= vpage_end))
                {
                    candidate = (vpage_end + PAGE_SIZE - 1) & PAGE_MASK;
                    occupied = true;
                    break;
                }
            }
            spin_unlock(&owner->virt_queue->lock);
            if (occupied) continue;
        }
        for (uint64_t offset = 0; offset < aligned_length; offset += PAGE_SIZE)
        {
            if (translate_address(directory, candidate + offset) != 0)
            {
                candidate = ((candidate + offset) & PAGE_MASK) + PAGE_SIZE;
                occupied = true;
                break;
            }
        }

        if (!occupied) return candidate;
    }

    return 0;
}

void            page_init()
{
    uint64_t page_maps_size = memory_size / PAGE_SIZE * sizeof(page_t);
    page_maps               = (page_t *)early_alloc(page_maps_size);
    if (page_maps) memset(page_maps, 0, page_maps_size);
}

page_t *get_page(uint64_t addr)
{
    if (page_maps == NULL) return NULL;
    if (addr >= memory_size) return NULL;
    return page_maps + (addr / PAGE_SIZE);
}

void page_ref(page_t *page)
{
    if (page) page->refcount++;
}
void page_unref(page_t *page)
{
    if (page) page->refcount--;
}

bool page_can_free(page_t *page)
{
    return page ? page->refcount <= 0 : true;
}

void address_ref(uint64_t addr)
{
    page_ref(get_page(addr));
}
void address_unref(uint64_t addr)
{
    page_unref(get_page(addr));
}

bool address_can_free(uint64_t addr)
{
    return page_can_free(get_page(addr));
}

static bool is_huge_page(page_table_entry_t *entry)
{
    return (((uint64_t)entry->value) & PTE_HUGE) != 0;
}

static uint64_t page_entry_value(page_directory_t *directory, uint64_t addr)
{
    if (directory == NULL || directory->table == NULL) return 0;

    uint64_t l4_index = (addr >> 39) & ENTRY_MASK;
    uint64_t l3_index = (addr >> 30) & ENTRY_MASK;
    uint64_t l2_index = (addr >> 21) & ENTRY_MASK;
    uint64_t l1_index = (addr >> 12) & ENTRY_MASK;

    page_table_t *l4 = directory->table;
    uint64_t e4 = l4->entries[l4_index].value;
    if (!(e4 & PTE_PRESENT) || (e4 & PTE_HUGE)) return e4;
    page_table_t *l3 = (page_table_t *)phys_to_virt(e4 & PTE_ADDR_MASK);
    uint64_t e3 = l3->entries[l3_index].value;
    if (!(e3 & PTE_PRESENT) || (e3 & PTE_HUGE)) return e3;
    page_table_t *l2 = (page_table_t *)phys_to_virt(e3 & PTE_ADDR_MASK);
    uint64_t e2 = l2->entries[l2_index].value;
    if (!(e2 & PTE_PRESENT) || (e2 & PTE_HUGE)) return e2;
    page_table_t *l1 = (page_table_t *)phys_to_virt(e2 & PTE_ADDR_MASK);
    return l1->entries[l1_index].value;
}

static void dump_pf_frame_words(struct X64_REGS *frame)
{
    const uint64_t *words = (const uint64_t *)frame;
    write_serial_string("[pf-frame]");
    for (size_t i = 0; i < sizeof(struct X64_REGS) / sizeof(uint64_t); i++)
    {
        if ((i % 4) == 0)
        {
            write_serial_string("\n  +");
            write_serial_hex(i * sizeof(uint64_t));
            write_serial_string(":");
        }
        write_serial_string(" ");
        write_serial_hex(words[i]);
    }
    write_serial_string("\n");
}

static void dump_user_code_bytes(page_directory_t *pagedir, uint64_t rip)
{
    if (pagedir == NULL || rip < 16) return;

    uint64_t start = rip - 16;
    write_serial_string("[pfcode] start=");
    write_serial_hex(start);
    write_serial_string(" bytes:");
    for (size_t i = 0; i < 48; i++)
    {
        uint8_t byte = 0;
        write_serial_string((i % 16) == 0 ? "\n  " : " ");
        if (copy_from_user_pagedir(pagedir, &byte, (const void *)(start + i), sizeof(byte)))
            write_serial_hex(byte);
        else
            write_serial_string("??");
    }
    write_serial_string("\n");
}

static void dump_user_qwords(page_directory_t *pagedir, const char *tag, uint64_t addr, size_t count)
{
    if (pagedir == NULL || tag == NULL || addr == 0) return;

    write_serial_string(tag);
    write_serial_string(" start=");
    write_serial_hex(addr);
    for (size_t i = 0; i < count; i++)
    {
        uint64_t value = 0;
        write_serial_string((i % 4) == 0 ? "\n  " : " ");
        write_serial_string("+");
        write_serial_hex(i * sizeof(uint64_t));
        write_serial_string("=");
        if (copy_from_user_pagedir(pagedir, &value, (const void *)(addr + i * sizeof(uint64_t)), sizeof(value)))
            write_serial_hex(value);
        else
            write_serial_string("??");
    }
    write_serial_string("\n");
}

static void dump_user_frame_chain(page_directory_t *pagedir, uint64_t rbp)
{
    if (pagedir == NULL || rbp == 0) return;

    write_serial_string("[pf-user-bt] rbp=");
    write_serial_hex(rbp);
    write_serial_string("\n");

    uint64_t current = rbp;
    for (size_t depth = 0; depth < 12; depth++)
    {
        uint64_t next = 0;
        uint64_t ret  = 0;
        bool ok_next = copy_from_user_pagedir(pagedir, &next, (const void *)current, sizeof(next));
        bool ok_ret  = copy_from_user_pagedir(pagedir, &ret, (const void *)(current + sizeof(uint64_t)), sizeof(ret));

        write_serial_string("  #");
        write_serial_dec(depth);
        write_serial_string(" frame=");
        write_serial_hex(current);
        write_serial_string(" next=");
        if (ok_next) write_serial_hex(next);
        else write_serial_string("??");
        write_serial_string(" ret=");
        if (ok_ret)
        {
            write_serial_hex(ret);
            if (ret >= USER_MMAP_START)
            {
                write_serial_string(" off_mmap=");
                write_serial_hex(ret - USER_MMAP_START);
            }
            if (ret >= 0x40000000ULL && ret < USER_MMAP_START)
            {
                write_serial_string(" off_main=");
                write_serial_hex(ret - 0x40000000ULL);
            }
        }
        else
        {
            write_serial_string("??");
        }
        write_serial_string("\n");

        if (!ok_next || !ok_ret || next <= current || next - current > (1ULL << 20)) break;
        current = next;
    }
}

static void dump_user_mapping_probe(page_directory_t *pagedir, const char *tag, uint64_t addr)
{
    if (pagedir == NULL || tag == NULL || addr == 0) return;

    uint64_t phys = translate_address(pagedir, addr);
    uint64_t pte  = page_entry_value(pagedir, addr);
    write_serial_string(tag);
    write_serial_string(" va=");
    write_serial_hex(addr);
    write_serial_string(" phys=");
    write_serial_hex(phys);
    write_serial_string(" pte=");
    write_serial_hex(pte);
    write_serial_string(" bytes=");

    for (size_t i = 0; i < 8; i++)
    {
        uint8_t byte = 0;
        write_serial_string(i == 0 ? "" : " ");
        if (copy_from_user_pagedir(pagedir, &byte, (const void *)(addr + i), sizeof(byte)))
            write_serial_hex(byte);
        else
            write_serial_string("??");
    }
    write_serial_string("\n");
}

extern "C" void handle_page_fault(struct X64_REGS *frame, uint64_t error_code)
{
    close_interrupt;
    disable_scheduler();
    tcb_t current_task = get_current_task();
    uint64_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));

    const char *error_msg = !(error_code & 0x1) ? "NotPresent"
                            : error_code & 0x2  ? "WriteError"
                            : error_code & 0x4  ? "UserMode"
                            : error_code & 0x8  ? "ReservedBitsSet"
                            : error_code & 0x10 ? "DecodeAddress"
                                                : "Unknown";

    // 长大后在学习吧！
    if (current_task != NULL)
    {
        pcb_t current_proc = current_task->parent_group;
        if (current_proc != NULL && current_proc->task_level == TASK_APPLICATION_LEVEL)
        {
            errno_t status = lazy_tryalloc(current_proc, faulting_address);
            if (status == 0)
            {
                enable_scheduler();
                open_interrupt;
                return;
            }
            if (try_map_user_stack_fault(current_task, faulting_address))
            {
                enable_scheduler();
                open_interrupt;
                return;
            }
            goto err;
        }
    }
err:;
    write_serial_string("Page fault virtual address (cr2): ");
    write_serial_hex(faulting_address);
    write_serial_string(" ");
    write_serial_hex(frame->rip);
    write_serial_string("\n");

    write_serial_string("Type: ");
    write_serial_string(error_msg);
    write_serial_string("\n");
    write_serial_fmt("PF cpu lapic=%d gs=0x%x kgs=0x%x current_cpu=0x%x\n",
                     lapic_id(),
                     read_gsbase(),
                     read_kgsbase(),
                     (uint64_t)get_current_cpu());

    if (current_task != NULL)
    {
        uint16_t fs_selector = 0;
        __asm__ __volatile__("mov %%fs, %0" : "=r"(fs_selector));
        write_serial_fmt("Fault Thread Name: %s 0x%x\n", current_task->name, (uint64_t)current_task);
        write_serial_fmt("TID: %d\n", current_task->tid);
        write_serial_string("[pfraw] cr2=");
        write_serial_hex(faulting_address);
        write_serial_string(" rip=");
        write_serial_hex(frame->rip);
        write_serial_string(" rsp=");
        write_serial_hex(frame->rsp);
        write_serial_string(" rax=");
        write_serial_hex(frame->rax);
        write_serial_string(" rbx=");
        write_serial_hex(frame->rbx);
        write_serial_string(" rdx=");
        write_serial_hex(frame->rdx);
        write_serial_string(" rbp=");
        write_serial_hex(frame->rbp);
        write_serial_string("\n");
        write_serial_fmt("[fsbase] pf task=%s tid=%llu tcb=0x%llx hw=0x%llx fs_sel=0x%llx rip=0x%llx rsp=0x%llx err=0x%llx\n",
                         current_task->name, current_task->tid, current_task->fs_base, read_fsbase(),
                         fs_selector, frame->rip, frame->rsp, error_code);
        write_serial_fmt("[pfregs] rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx\n",
                         frame->rax, frame->rbx, frame->rcx, frame->rdx, frame->rbp, frame->rsi, frame->rdi);
        write_serial_fmt("[pfregs] r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                         frame->r8, frame->r9, frame->r10, frame->r11, frame->r12, frame->r13, frame->r14,
                         frame->r15);
        if (current_task->parent_group != NULL)
        {
            uint64_t fs_canary_addr = read_fsbase() + 0x28;
            uint64_t fs_canary_phys = translate_address(current_task->parent_group->pagedir, fs_canary_addr);
            uint64_t fs_canary_pte  = page_entry_value(current_task->parent_group->pagedir, fs_canary_addr);
            uint64_t canary_value   = fs_canary_phys != 0 ? *(uint64_t *)phys_to_virt(fs_canary_phys) : 0;
            write_serial_fmt("[pfcheck] fs28=0x%llx phys=0x%llx pte=0x%llx val=0x%llx cr2_pte=0x%llx\n",
                             fs_canary_addr, fs_canary_phys, fs_canary_pte, canary_value,
                             page_entry_value(current_task->parent_group->pagedir, faulting_address));
            dump_user_code_bytes(current_task->parent_group->pagedir, frame->rip);
            dump_user_qwords(current_task->parent_group->pagedir, "[pf-user-rsp]", frame->rsp, 12);
            if (frame->rbp >= 0x40)
                dump_user_qwords(current_task->parent_group->pagedir, "[pf-user-rbp]", frame->rbp - 0x40, 16);
            dump_user_frame_chain(current_task->parent_group->pagedir, frame->rbp);
            dump_user_mapping_probe(current_task->parent_group->pagedir, "[pf-map-rip]",
                                    frame->rip & PAGE_MASK);
            if (frame->rip >= USER_MMAP_START)
                dump_user_mapping_probe(current_task->parent_group->pagedir, "[pf-map-libc-base]",
                                        USER_MMAP_START);
            dump_user_mapping_probe(current_task->parent_group->pagedir, "[pf-map-main-base]", 0x40000000ULL);
        }
        dump_pf_frame_words(frame);
    }
    else
    {
        write_serial_string("Fault Thread Name: <none>\n");
        write_serial_string("TID: <none>\n");
    }

    backtrace(frame);

    if (error_code & 0x4)
    {
        ulog_err("Page Fault (0x0000000e)");
        if (current_task != NULL && current_task->parent_group != NULL)
        {
            kill_proc(current_task->parent_group, 128 + 11, true);
            enable_scheduler();
            open_interrupt;
            scheduler_yield();
            while (1) { __asm__ __volatile__("hlt"); }
        }
        if (current_task != NULL && current_task->task_level == TASK_APPLICATION_LEVEL)
        {
            kill_thread(current_task);
            enable_scheduler();
            open_interrupt;
            scheduler_yield();
        }
        enable_scheduler();
        open_interrupt;
        return;
    }

    while (1)
    {
        __asm__ __volatile__("hlt");
    }
}

void page_table_clear(page_table_t *table)
{
    for (int i = 0; i < 512; i++)
    {
        table->entries[i].value = 0;
    }
}

bool page_table_get_flags(page_directory_t *directory, uint64_t addr, uint64_t *out_flags)
{
    if (directory == NULL || directory->table == NULL || out_flags == NULL) return false;

    uint64_t l4_index = (((addr >> 39)) & 0x1FF);
    uint64_t l3_index = (((addr >> 30)) & 0x1FF);
    uint64_t l2_index = (((addr >> 21)) & 0x1FF);
    uint64_t l1_index = (((addr >> 12)) & 0x1FF);

    page_table_t *pml4 = directory->table;

    if (!(pml4->entries[l4_index].value & PTE_PRESENT)) return false;
    if (pml4->entries[l4_index].value & PTE_HUGE)
    {
        *out_flags = pml4->entries[l4_index].value & 0xFFF;
        return true;
    }

    page_table_t *pdpt = (page_table_t *)phys_to_virt(pml4->entries[l4_index].value & PTE_ADDR_MASK);
    if (pdpt == NULL) return false;
    if (!(pdpt->entries[l3_index].value & PTE_PRESENT)) return false;
    if (pdpt->entries[l3_index].value & PTE_HUGE)
    {
        *out_flags = pdpt->entries[l3_index].value & 0xFFF;
        return true;
    }

    page_table_t *pd = (page_table_t *)phys_to_virt(pdpt->entries[l3_index].value & PTE_ADDR_MASK);
    if (pd == NULL) return false;
    if (!(pd->entries[l2_index].value & PTE_PRESENT)) return false;
    if (pd->entries[l2_index].value & PTE_HUGE)
    {
        *out_flags = pd->entries[l2_index].value & 0xFFF;
        return true;
    }

    page_table_t *pt = (page_table_t *)phys_to_virt(pd->entries[l2_index].value & PTE_ADDR_MASK);
    if (pt == NULL) return false;
    if (!(pt->entries[l1_index].value & PTE_PRESENT)) return false;

    *out_flags = pt->entries[l1_index].value & 0xFFF;
    return true;
}

bool page_table_update_flags(page_directory_t *directory, uint64_t addr, uint64_t new_flags)
{
    if (directory == NULL || directory->table == NULL) return false;

    page_table_t *pml4     = directory->table;
    uint64_t      l4_index = (((addr >> 39)) & 0x1FF);
    uint64_t      l3_index = (((addr >> 30)) & 0x1FF);
    uint64_t      l2_index = (((addr >> 21)) & 0x1FF);
    uint64_t      l1_index = (((addr >> 12)) & 0x1FF);

    if (!(pml4->entries[l4_index].value & PTE_PRESENT)) return false;
    if (pml4->entries[l4_index].value & PTE_HUGE)
    {
        uint64_t phys                 = pml4->entries[l4_index].value & PTE_ADDR_MASK;
        pml4->entries[l4_index].value = phys | new_flags;
        flush_tlb(addr);
        return true;
    }

    page_table_t *pdpt = (page_table_t *)phys_to_virt(pml4->entries[l4_index].value & PTE_ADDR_MASK);
    if (pdpt == NULL) return false;
    if (!(pdpt->entries[l3_index].value & PTE_PRESENT)) return false;
    if (pdpt->entries[l3_index].value & PTE_HUGE)
    {
        uint64_t phys                 = pdpt->entries[l3_index].value & PTE_ADDR_MASK;
        pdpt->entries[l3_index].value = phys | new_flags;
        flush_tlb(addr);
        return true;
    }

    page_table_t *pd = (page_table_t *)phys_to_virt(pdpt->entries[l3_index].value & PTE_ADDR_MASK);
    if (pd == NULL) return false;
    if (!(pd->entries[l2_index].value & PTE_PRESENT)) return false;
    if (pd->entries[l2_index].value & PTE_HUGE)
    {
        uint64_t phys               = pd->entries[l2_index].value & PTE_ADDR_MASK;
        pd->entries[l2_index].value = phys | new_flags;
        flush_tlb(addr);
        return true;
    }

    page_table_t *pt = (page_table_t *)phys_to_virt(pd->entries[l2_index].value & PTE_ADDR_MASK);
    if (pt == NULL) return false;
    if (!(pt->entries[l1_index].value & PTE_PRESENT)) return false;

    uint64_t phys               = pt->entries[l1_index].value & PTE_ADDR_MASK;
    uint64_t keep               = pt->entries[l1_index].value & PTE_FRAME_ALLOCATED;
    pt->entries[l1_index].value = phys | keep | new_flags;

    flush_tlb(addr);
    return true;
}

page_table_t *page_table_create(page_table_entry_t *entry, bool user)
{
    if (entry->value == (uint64_t)NULL)
    {
        uint64_t frame = alloc_frames(1);
        // write_serial_fmt("oiiaoiia 0x%x\n",(uint64_t)frame);
        entry->value        = frame | PTE_PRESENT | PTE_WRITEABLE | (user ? PTE_USER : 0);
        page_table_t *table = (page_table_t *)phys_to_virt(entry->value & PTE_ADDR_MASK);
        page_table_clear(table);
        return table;
    }
    entry->value        |= (user ? PTE_USER : 0);
    page_table_t *table  = (page_table_t *)phys_to_virt(entry->value & PTE_ADDR_MASK);
    return table;
}

page_directory_t *get_kernel_pagedir()
{
    return &kernel_page_dir;
}

extern volatile bool is_scheduler;

page_directory_t gdc_temp_pdt;

page_directory_t *get_current_directory()
{
    gdc_temp_pdt.table = (page_table_t *)phys_to_virt(get_cr3());
    return &gdc_temp_pdt;
}

EXPORT_SYMBOL(get_current_directory);

static page_table_t *copy_page_table_recursive(page_table_t *source_table, int level, bool all_copy, bool kernel_space)
{
    if (source_table == NULL) return NULL;
    if (level <= 1)
    {
        if (kernel_space) { return source_table; }

        uint64_t      frame          = alloc_frames(1);
        page_table_t *new_page_table = (page_table_t *)phys_to_virt(frame);
        page_table_clear(new_page_table);
        for (int i = 0; i < 512; i++)
        {
            uint64_t source_entry = source_table->entries[i].value;
            if (!(source_entry & PTE_PRESENT))
            {
                new_page_table->entries[i].value = 0;
                continue;
            }

            if (source_entry & PTE_HUGE)
            {
                new_page_table->entries[i].value = source_entry;
                continue;
            }

            // fork needs a private snapshot of every present user page. Some
            // mmap/lazy pages do not carry PTE_FRAME_ALLOCATED, but sharing
            // them lets the parent corrupt the child's heap/stack view.
            uint64_t source_phys = source_entry & PTE_ADDR_MASK;
            uint64_t new_phys    = alloc_frames(1);
            if (new_phys == 0) { continue; }

            memcpy((void *)phys_to_virt(new_phys), (const void *)phys_to_virt(source_phys), PAGE_SIZE);
            new_page_table->entries[i].value = (source_entry & ~PTE_ADDR_MASK) | new_phys | PTE_FRAME_ALLOCATED;
        }
        return new_page_table;
    }

    uint64_t      phy_frame = alloc_frames(1);
    page_table_t *new_table = (page_table_t *)(phys_to_virt(phy_frame));
    page_table_clear(new_table);
    for (uint64_t i = 0; i < (all_copy ? 512 : (level == 4 ? 256 : 512)); i++)
    {
        if (!(source_table->entries[i].value & PTE_PRESENT))
        {
            new_table->entries[i].value = 0;
            continue;
        }
        if (source_table->entries[i].value & PTE_HUGE)
        {
            // 指向一块2M的页
            new_table->entries[i].value = source_table->entries[i].value;
            continue;
        }

        // 指向一个4K的页表
        page_table_t *source_page_table_next =
            (page_table_t *)phys_to_virt(source_table->entries[i].value & PTE_ADDR_MASK);
        page_table_t *new_page_table = copy_page_table_recursive(source_page_table_next, level - 1, all_copy,
                                                                 level != 4 ? kernel_space : i >= 256);
        new_table->entries[i].value =
            (uint64_t)virt_to_phys((uint64_t)new_page_table) | (source_table->entries[i].value & ~PTE_ADDR_MASK);
    }
    return new_table;
}

static void free_page_table_recursive(page_table_t *table, int level)
{
    if (table == NULL) return;
    if (level <= 1)
    {
        for (int i = 0; i < 512; i++)
        {
            uint64_t entry = table->entries[i].value;
            if ((entry & PTE_PRESENT) && (entry & PTE_FRAME_ALLOCATED))
            {
                free_frame(entry & PTE_ADDR_MASK);
                table->entries[i].value = 0;
            }
        }
        free_frame((uint64_t)virt_to_phys((uint64_t)table));
        return;
    }

    for (int i = 0; i < (level == 4 ? 256 : 512); i++)
    {
        uint64_t entry = table->entries[i].value;
        if (!(entry & PTE_PRESENT)) continue;
        if (entry & PTE_HUGE) continue;//竟然没有检查,还是保险点吧！！！！

        page_table_t *page_table_next = (page_table_t *)(phys_to_virt(table->entries[i].value & PTE_ADDR_MASK));
        free_page_table_recursive(page_table_next, level - 1);
    }
    free_frame((uint64_t)virt_to_phys((uint64_t)table));
}

page_directory_t *clone_page_directory(page_directory_t *dir, bool all_copy)
{
    spin_lock(&page_lock);
    page_directory_t *new_directory = (page_directory_t *)(malloc(sizeof(page_directory_t)));
    if (new_directory == NULL) return NULL;
    new_directory->table = copy_page_table_recursive(dir->table, 4, all_copy, false);
    if (!all_copy) memcpy((uint64_t *)new_directory->table + 256, (uint64_t *)dir->table + 256, PAGE_SIZE / 2);
    spin_unlock(&page_lock);
    return new_directory;
}

void free_page_directory(page_directory_t *dir)
{
    spin_lock(&page_lock);
    free_page_table_recursive(dir->table, 4);
    free(dir);
    spin_unlock(&page_lock);
}

void unmap_page(page_directory_t *directory, uint64_t vaddr)
{
    if (directory == NULL || directory->table == NULL) return;

    uint64_t l4_index = (((vaddr >> 39)) & 0x1FF);
    uint64_t l3_index = (((vaddr >> 30)) & 0x1FF);
    uint64_t l2_index = (((vaddr >> 21)) & 0x1FF);
    uint64_t l1_index = (((vaddr >> 12)) & 0x1FF);

    page_table_t *l4_table = directory->table;
    if (!(l4_table->entries[l4_index].value & PTE_PRESENT)) return;
    if (l4_table->entries[l4_index].value & PTE_HUGE) return;

    page_table_t *l3_table =
        (page_table_t *)(phys_to_virt((&(l4_table->entries[l4_index]))->value & PTE_ADDR_MASK));
    if (l3_table == NULL) return;
    if (!(l3_table->entries[l3_index].value & PTE_PRESENT)) return;
    if (l3_table->entries[l3_index].value & PTE_HUGE) return;

    page_table_t *l2_table =
        (page_table_t *)(phys_to_virt((&(l3_table->entries[l3_index]))->value & PTE_ADDR_MASK));
    if (l2_table == NULL) return;
    if (!(l2_table->entries[l2_index].value & PTE_PRESENT)) return;
    if (l2_table->entries[l2_index].value & PTE_HUGE) return;

    page_table_t *l1_table =
        (page_table_t *)(phys_to_virt((&(l2_table->entries[l2_index]))->value & PTE_ADDR_MASK));
    if (l1_table == NULL) return;
    if (!(l1_table->entries[l1_index].value & PTE_PRESENT)) return;

    if (l1_table->entries[l1_index].value & PTE_FRAME_ALLOCATED)
        free_frame(l1_table->entries[l1_index].value & PTE_ADDR_MASK);
    l1_table->entries[l1_index].value = 0;
    flush_tlb(vaddr);
}

void unmap_page_range(page_directory_t *directory, uint64_t vaddr, uint64_t size)
{
    for (uint64_t va = vaddr; va < vaddr + size; va += PAGE_SIZE)
    {
        unmap_page(directory, va);
    }
}

void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
    uint64_t l4_index = (((addr >> 39)) & 0x1FF);
    uint64_t l3_index = (((addr >> 30)) & 0x1FF);
    uint64_t l2_index = (((addr >> 21)) & 0x1FF);
    uint64_t l1_index = (((addr >> 12)) & 0x1FF);

    page_table_t *l4_table = directory->table;
    page_table_t *l3_table = page_table_create(&(l4_table->entries[l4_index]), !!(flags & PTE_USER));
    page_table_t *l2_table = page_table_create(&(l3_table->entries[l3_index]), !!(flags & PTE_USER));
    page_table_t *l1_table = page_table_create(&(l2_table->entries[l2_index]), !!(flags & PTE_USER));

    if (!frame) { serial_wprintf("WARNING: Invalid frame addr: 0\n"); }

    l1_table->entries[l1_index].value = (frame & PTE_ADDR_MASK) | flags;

    flush_tlb(addr);
}

EXPORT_SYMBOL(page_map_to);

void switch_process_page_directory(page_directory_t *dir)
{
    bool was_scheduler_enabled = is_scheduler;
    bool was_interrupt_enabled = are_interrupts_enabled();

    disable_scheduler();
    close_interrupt;
    pcb_t pcb    = get_current_task()->parent_group;
    pcb->pagedir = dir;
    switch_page_directory(dir);

    if (was_scheduler_enabled) enable_scheduler();
    if (was_interrupt_enabled && !no_interrupt) open_interrupt;
}

void switch_page_directory(page_directory_t *dir)
{
    // if (cpu->ready) {
    //     cpu->directory = dir;
    // } else {
    current_directory = dir;
    // }
    page_table_t *physical_table = (page_table_t *)(virt_to_phys((uint64_t)((page_table_t *)((uint64_t)dir->table))));
    // write_serial_string("Switch Page Directory:\nPhysAddr:");
    // write_serial_hex((uint64_t)(physical_table));
    // write_serial_string("\nVirtAddr:");
    // write_serial_hex((uint64_t)(phys_to_virt((uint64_t)(physical_table))));
    // write_serial_string("\n");
    __asm__ volatile("mov %0, %%cr3" : : "r"(physical_table));
}

uint64_t page_alloc_random(page_directory_t *directory, uint64_t length, uint64_t flags)
{
    if (length == 0) return 0;

    uint64_t aligned_length = page_align_up(length);
    if (aligned_length == 0) return 0;

    size_t frame_count = aligned_length / PAGE_SIZE;
    uint64_t frame = alloc_frames(frame_count);
    if (frame == 0) return 0;

    uint64_t addr = frame;
    if (flags & PTE_USER)
    {
        pcb_t owner = find_process_by_pagedir(directory);
        uint64_t start = (owner != NULL && owner->mmap_start >= USER_MMAP_START) ? owner->mmap_start : USER_MMAP_START;
        addr = find_free_user_range(directory, start, aligned_length);
        if (addr == 0 && start != USER_MMAP_START)
        {
            addr = find_free_user_range(directory, USER_MMAP_START, aligned_length);
        }
        if (addr == 0)
        {
            free_frames(frame, frame_count);
            return 0;
        }
        if (owner != NULL) owner->mmap_start = addr + aligned_length;
    }

    for (uint64_t i = 0; i < aligned_length; i += PAGE_SIZE)
    {
        page_map_to(directory, addr + i, frame + i, flags | PTE_FRAME_ALLOCATED);
    }
    return addr;
}

uint64_t page_reserve_user_range(page_directory_t *directory, uint64_t length)
{
    if (directory == NULL || length == 0) return 0;

    uint64_t aligned_length = page_align_up(length);
    if (aligned_length == 0) return 0;

    pcb_t owner = find_process_by_pagedir(directory);
    uint64_t start = (owner != NULL && owner->mmap_start >= USER_MMAP_START) ? owner->mmap_start : USER_MMAP_START;
    uint64_t addr = find_free_user_range(directory, start, aligned_length);
    if (addr == 0 && start != USER_MMAP_START)
    {
        addr = find_free_user_range(directory, USER_MMAP_START, aligned_length);
    }
    if (addr == 0) return 0;

    if (owner != NULL) owner->mmap_start = addr + aligned_length;
    if (owner != NULL &&
        !lazy_infoalloc(owner, addr, aligned_length, PTE_USER | PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE, 0))
    {
        return 0;
    }
    return addr;
}

void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    for (uint64_t i = 0; i < length; i += 0x1000)
    {
        uint64_t var = (uint64_t)addr + i;
        uint64_t frame = alloc_frames(1);
        if (frame == 0) continue;
        memset((void *)phys_to_virt(frame), 0, PAGE_SIZE);
        page_map_to(directory, var, frame, flags | PTE_FRAME_ALLOCATED);
    }
}

void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags)
{
    for (uint64_t i = 0; i < length; i += 0x1000)
    {
        uint64_t var = (uint64_t)phys_to_virt(frame + i);
        page_map_to(directory, var, frame + i, flags);
    }
}

void page_map_range(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t length, uint64_t flags)
{
    for (uint64_t i = 0; i < length; i += 0x1000)
    {
        uint64_t var = (uint64_t)addr + i;
        page_map_to(directory, var, frame + i, flags);
    }
}

uint64_t translate_address(page_directory_t *page_dir, uint64_t virtual_address)
{
    // 计算各级页表索引
    uint64_t pml4_index = (virtual_address >> 39) & ENTRY_MASK;
    uint64_t pdp_index  = (virtual_address >> 30) & ENTRY_MASK;
    uint64_t pd_index   = (virtual_address >> 21) & ENTRY_MASK;
    uint64_t pt_index   = (virtual_address >> 12) & ENTRY_MASK;

    // 获取PML4表
    page_table_t      *pml4_table = page_dir->table;
    page_table_entry_t pml4_entry = pml4_table->entries[pml4_index];

    // 检查PML4条目是否存在
    if (!(pml4_entry.value & PTE_PRESENT)) { return 0; }

    // 如果PML4条目指向的是1GB大页
    if (ARCH_PT_IS_LARGE(pml4_entry.value))
    {
        uint64_t base_addr = pml4_entry.value & ~((1UL << 30) - 1);
        return base_addr + (virtual_address & ((1UL << 30) - 1));
    }

    // 获取PDPT表
    page_table_t      *pdpt_table = (page_table_t *)phys_to_virt(pml4_entry.value & PTE_ADDR_MASK);
    page_table_entry_t pdpt_entry = pdpt_table->entries[pdp_index];

    // 检查PDPT条目是否存在
    if (!(pdpt_entry.value & PTE_PRESENT)) { return 0; }

    // 如果PDPT条目指向的是1GB大页
    if (ARCH_PT_IS_LARGE(pdpt_entry.value))
    {
        uint64_t base_addr = pdpt_entry.value & (PTE_ADDR_MASK & ~((1ULL << 30) - 1));
        return base_addr + (virtual_address & ((1UL << 30) - 1));
    }

    // 获取PD表
    page_table_t      *pd_table = (page_table_t *)phys_to_virt(pdpt_entry.value & PTE_ADDR_MASK);
    page_table_entry_t pd_entry = pd_table->entries[pd_index];

    // 检查PD条目是否存在
    if (!(pd_entry.value & PTE_PRESENT)) { return 0; }

    // 如果PD条目指向的是2MB大页
    if (ARCH_PT_IS_LARGE(pd_entry.value))
    {
        uint64_t base_addr = pd_entry.value & (PTE_ADDR_MASK & ~((1ULL << 21) - 1));
        return base_addr + (virtual_address & ((1UL << 21) - 1));
    }

    // 获取PT表
    page_table_t      *pt_table = (page_table_t *)phys_to_virt(pd_entry.value & PTE_ADDR_MASK);
    page_table_entry_t pt_entry = pt_table->entries[pt_index];

    // 检查PT条目是否存在
    if (!(pt_entry.value & PTE_PRESENT)) { return 0; }

    // 返回4KB页的物理地址
    return (pt_entry.value & PTE_ADDR_MASK) + (virtual_address & ~PAGE_MASK);
}

void switch_xsk_page_directory()
{
    page_directory_t *new_directory = clone_page_directory(&kernel_page_dir, true);
    kernel_page_dir.table           = new_directory->table;
    free(new_directory);
    switch_page_directory(&kernel_page_dir);
    double_fault_page = get_cr3();
    current_directory = &kernel_page_dir;
}

void page_setup()
{
    page_table_t *kernel_page_table = (page_table_t *)phys_to_virt(get_cr3());
    kernel_page_dir                 = (page_directory_t){.table = kernel_page_table};
    double_fault_page               = get_cr3();
    current_directory               = &kernel_page_dir;
}

EXPORT_SYMBOL(page_map_range);
EXPORT_SYMBOL(page_map_range_to);
EXPORT_SYMBOL(page_map_range_to_random);
