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
#include <syscall/fd_lifetime.h>
#include <block_access.h>
#include <installer/jobs.h>
#include <procfs.h>

extern bool no_interrupt; // 涓簍rue鏃朵唬琛ㄤ弗绂佸紑鍚腑鏂?

static bool process_is_current_on_any_cpu(pcb_t process)
{
    return scheduler_process_referenced(process);
}

static pcb_t find_reapable_child(pcb_t parent)
{
    if (parent == NULL || parent->child_pcb == NULL) return NULL;

    pcb_t target = NULL;
    spin_lock(&parent->child_pcb->lock);
    for (lock_node *node = parent->child_pcb->head; node != NULL; node = node->next)
    {
        pcb_t child = (pcb_t)node->data;
        if (child != NULL && child->status == DEATH && !child->wait_owner &&
            child->fd_cleanup_state != 3 && child->fd_cleanup_state != 4 && !process_is_current_on_any_cpu(child))
        {
            __atomic_store_n(&child->wait_owner, get_current_task(), __ATOMIC_RELEASE);
            __atomic_store_n(&child->status, WAIT, __ATOMIC_RELEASE);
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
        }
        block_access_reap_abandoned();
        installer::jobs_reap_abandoned();
        process_reap_wait_claims();
        reap_quiesced_file_descriptors();
        fd_reap_abandoned_operations();
        procfs_reap_retired();
        pcb_t target = find_reapable_child(kernel_group);
        if (target != NULL) process_complete_wait(target);
        scheduler_yield();
    }
}

void init_reaper()
{
    create_kernel_thread((void *)reaper_thread, NULL, "Process Reaper", NULL);
}
