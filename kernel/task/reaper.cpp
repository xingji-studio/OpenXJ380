#include "cpu/regio.h"
#include "krlibc.h"
#include "mm/heap.h"
#include "mm/page.h"
#include "proto.hpp"
#include "task/pcb.h"
#include "task/scheduler.h"
#include <cpu/longm.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <smp/smp.h>

extern bool no_interrupt; // 涓簍rue鏃朵唬琛ㄤ弗绂佸紑鍚腑鏂?

static bool process_is_current_on_any_cpu(pcb_t process)
{
    if (process == NULL) return false;

    for (size_t i = 0; i < get_cpu_num(); i++)
    {
        PROCESSOR_INFO *cpu = get_cpu(i);
        tcb_t task = cpu != NULL ? cpu->current_task : NULL;
        if (task != NULL && task->parent_group == process) return true;
    }

    return false;
}

static pcb_t find_reapable_child(pcb_t parent)
{
    if (parent == NULL || parent->child_pcb == NULL) return NULL;

    pcb_t target = NULL;
    spin_lock(&parent->child_pcb->lock);
    for (lock_node *node = parent->child_pcb->head; node != NULL; node = node->next)
    {
        pcb_t child = (pcb_t)node->data;
        if (child != NULL && !child->is_initial_program && child->status == DEATH &&
            !process_is_current_on_any_cpu(child))
        {
            target = child;
            break;
        }
    }
    spin_unlock(&parent->child_pcb->lock);
    return target;
}

void reaper_thread()
{
    while (true)
    {
        if (!no_interrupt)
        {
            open_interrupt;
            enable_scheduler();
        }
        pcb_t target = find_reapable_child(kernel_group);
        if (target != NULL) kill_proc(target, 0, false);
        scheduler_yield();
    }
}

void init_reaper()
{
    create_kernel_thread((void *)reaper_thread, NULL, "Process Reaper", NULL);
}
