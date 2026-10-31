// SPDX-License-Identifier: Apache-2.0
#define ALL_IMPLEMENTATION
#include <procfs.h>
#include <efi/boot.h>
#include <errno.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <fs/vfs/vfs.h>
#include <mm/frame.h>
#include <syscall/syscall.h>
#include <proto.hpp>
#include <user/user.h>
#include <smp/smp.h>
#include <cpu/regio.h>
#include <syscall/fd_lifetime.h>
#include <user/permissions.h>

static int procfs_id     = 0;
static int proc_self_id  = 0;
static tcb_t procfs_bootstrap_owner;
static uint64_t procfs_bootstrap_tid;
vfs_node_t procfs_root   = NULL;
spin_t     procfs_oplock = SPIN_INIT;
extern lock_queue   *pcb_group_queue;
extern XSK_SMP_INFO *xsi;

// A process file can outlive both its name and its PCB. Context references keep
// that identity alive, while operation tickets alone retain access to the PCB.
// Collection withdraws the process first, retires its context, and waits for
// these bounded task-context operations before freeing process-owned storage.
struct ProcfsProcessContext
{
    pcb_t identity;
    pcb_t task;
    uint64_t pid;
    uint64_t references;
    uint64_t active;
    vfs_node_t node;
    vfs_node_t mount_pin;
    ProcfsProcessContext *next;
    bool retired;
    bool collecting;
};

struct ProcfsOperationRecord
{
    ProcfsProcessContext *process;
    pcb_t task;
    tcb_t owner;
    ProcfsOperationRecord *next;
};

static spin_t process_context_lock = SPIN_INIT;
static ProcfsProcessContext *process_contexts;
static ProcfsProcessContext *retired_contexts;
static ProcfsOperationRecord *process_operations;

static void procfs_destroy_context(ProcfsProcessContext *process)
{
    vfs_node_t mount = process->mount_pin;
    free(process);
    if (mount) vfs_close(mount);
}

class ProcfsSpinGuard
{
    spin_t *lock;
    uint64_t flags;
public:
    explicit ProcfsSpinGuard(spin_t *value) : lock(value), flags(get_rflags())
    {
        __asm__ volatile("cli" ::: "memory");
        spin_lock(lock);
    }
    ~ProcfsSpinGuard()
    {
        spin_unlock(lock);
        if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
    }
};

static void procfs_context_release(ProcfsProcessContext *process)
{
    if (!process) return;
    bool release = false;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        if (process->references) release = --process->references == 0;
    }
    if (release) procfs_destroy_context(process);
}

static bool procfs_context_retain(ProcfsProcessContext *process)
{
    ProcfsSpinGuard guard(&process_context_lock);
    if (!process || process->references == uint64_t(-1)) return false;
    ++process->references;
    return true;
}

static ProcfsOperationRecord *procfs_operation_acquire(ProcfsProcessContext *process)
{
    if (!get_current_task() || !scheduler_is_enabled()) return NULL;
    auto *record = static_cast<ProcfsOperationRecord *>(malloc(sizeof(ProcfsOperationRecord)));
    if (!record) return NULL;
    bool captured = false;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        if (process && !process->retired && process->task && process->references != uint64_t(-1) &&
            process->active != uint64_t(-1))
        {
            record->process = process;
            record->task = process->task;
            record->owner = get_current_task();
            record->next = process_operations;
            process_operations = record;
            ++process->references;
            ++process->active;
            captured = true;
        }
    }
    if (!captured) { free(record); return NULL; }
    return record;
}

static ProcfsOperationRecord *procfs_operation_for_pid(uint64_t pid)
{
    if (!pcb_group_queue || !get_current_task() || !scheduler_is_enabled()) return NULL;
    auto *candidate = static_cast<ProcfsProcessContext *>(calloc(1, sizeof(ProcfsProcessContext)));
    auto *record = static_cast<ProcfsOperationRecord *>(malloc(sizeof(ProcfsOperationRecord)));
    if (!candidate || !record) { free(candidate); free(record); return NULL; }
    bool captured = false, published = false;
    {
        // This lock proves the PCB has not been withdrawn by collection. No
        // allocator, filesystem callback or wait occurs in either locked scope.
        ProcfsSpinGuard list_guard(&pcb_group_queue->lock);
        pcb_t task = NULL;
        for (lock_node *entry = pcb_group_queue->head; entry; entry = entry->next)
        {
            auto *process = static_cast<pcb_t>(entry->data);
            if (process && process->pid == pid && process->status != DEATH && process->status != OUT &&
                __atomic_load_n(&process->fd_cleanup_state, __ATOMIC_ACQUIRE) < 3)
            {
                task = process;
                break;
            }
        }
        if (task)
        {
            ProcfsSpinGuard guard(&process_context_lock);
            ProcfsProcessContext *process = process_contexts;
            while (process && (process->identity != task || process->pid != pid)) process = process->next;
            if (!process)
            {
                process = candidate;
                process->identity = process->task = task;
                process->pid = pid;
                process->references = 1; // Published-context registry hold.
                process->next = process_contexts;
                process_contexts = process;
                published = true;
            }
            if (!process->retired && process->references != uint64_t(-1) && process->active != uint64_t(-1))
            {
                record->process = process;
                record->task = task;
                record->owner = get_current_task();
                record->next = process_operations;
                process_operations = record;
                ++process->references;
                ++process->active;
                captured = true;
            }
        }
    }
    if (!published) free(candidate);
    if (!captured) { free(record); return NULL; }
    return record;
}

static void procfs_operation_release(ProcfsOperationRecord *record)
{
    if (!record) return;
    ProcfsProcessContext *release = NULL;
    bool found = false;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        ProcfsOperationRecord **link = &process_operations;
        while (*link && *link != record) link = &(*link)->next;
        if (*link)
        {
            *link = record->next;
            ProcfsProcessContext *process = record->process;
            --process->active;
            if (!--process->references) release = process;
            found = true;
        }
    }
    if (found) free(record);
    if (release) procfs_destroy_context(release);
}

class ProcfsOperation
{
    ProcfsOperationRecord *record = NULL;
public:
    ProcfsOperation() = default;
    explicit ProcfsOperation(ProcfsProcessContext *process) { if (process) record = procfs_operation_acquire(process); }
    ~ProcfsOperation() { procfs_operation_release(record); }
    bool acquire(uint64_t pid) { record = procfs_operation_for_pid(pid); return record != NULL; }
    pcb_t get() const { return record ? record->task : NULL; }
    ProcfsProcessContext *context() const { return record ? record->process : NULL; }
    ProcfsOperation(const ProcfsOperation &) = delete;
    ProcfsOperation &operator=(const ProcfsOperation &) = delete;
};

static vfs_node_t procfs_ensure_process(ProcfsOperation &operation);

static size_t procfs_copy_content(void *addr, size_t offset, size_t size, const char *content, size_t content_len);
static void procfs_refresh_fd_dir(vfs_node_t fd_dir, ProcfsOperation &operation);
static bool procfs_fd_number(const char *name, size_t &number);
static vfs_node_t procfs_current_self_target()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return NULL;
    ProcfsOperation operation;
    if (!operation.acquire(task->parent_group->pid)) return NULL;
    return procfs_ensure_process(operation);
}

const char filesystems_content[] = //"nodev\tsysfs\n"
    "nodev\ttmpfs\n"
    "nodev\tproc\n"
    "nodev\tmodfs\n"
    "     \text4\n"
    "     \text3\n"
    "     \text2\n";

static int dummy() {
    return 0;
}

static int udummy() {
    return VFS_STATUS_FAILED;
}

static const char *procfs_kernel_cmdline()
{
    return "";
}

static const char *procfs_kernel_version()
{
    return "Linux version 6.6.30 (xj380@localhost) (x86_64-xj380-gcc) #1 SMP PREEMPT_DYNAMIC Wed May 6 00:00:00 CST 2026\n";
}

static const char *procfs_kernel_ostype()
{
    return "Linux\n";
}

static const char *procfs_kernel_osrelease()
{
    return "6.6.30\n";
}

static const char *procfs_kernel_hostname()
{
    return "localhost\n";
}

static size_t procfs_copy_task_cmdline(pcb_t task, void *addr, size_t offset, size_t size)
{
    if (addr == NULL || task == NULL) return VFS_STATUS_FAILED;

    size_t total = 0;
    if (task->argv != NULL && task->argc > 0)
    {
        for (size_t i = 0; i < task->argc && task->argv[i] != NULL; i++)
            total += strlen(task->argv[i]) + 1;
    }
    else if (task->cmdline != NULL)
    {
        total = strlen(task->cmdline);
    }

    if (offset >= total) return 0;

    size_t copied = 0;
    size_t pos = 0;
    uint8_t *out = (uint8_t *)addr;
    if (task->argv != NULL && task->argc > 0)
    {
        for (size_t i = 0; i < task->argc && task->argv[i] != NULL && copied < size; i++)
        {
            const char *arg = task->argv[i];
            size_t arg_len = strlen(arg) + 1;
            if (offset < pos + arg_len)
            {
                size_t in_arg = offset > pos ? offset - pos : 0;
                size_t chunk = MIN(size - copied, arg_len - in_arg);
                memcpy(out + copied, arg + in_arg, chunk);
                copied += chunk;
                offset += chunk;
            }
            pos += arg_len;
        }
        return copied;
    }

    return procfs_copy_content(addr, offset, size, task->cmdline ? task->cmdline : "", total);
}

static size_t procfs_task_cmdline_len(pcb_t task)
{
    if (task == NULL) return 0;
    if (task->argv != NULL && task->argc > 0)
    {
        size_t total = 0;
        for (size_t i = 0; i < task->argc && task->argv[i] != NULL; i++)
            total += strlen(task->argv[i]) + 1;
        return total;
    }
    return task->cmdline ? strlen(task->cmdline) : 0;
}

static char *proc_gen_meminfo(size_t *content_len)
{
    uint64_t total_kb = (frame_allocator.origin_frames * PAGE_SIZE) / 1024;
    uint64_t free_kb = (frame_allocator.usable_frames * PAGE_SIZE) / 1024;
    char *buffer = (char *)malloc(PAGE_SIZE);
    int len = sprintf(buffer,
                      "MemTotal:       %llu kB\n"
                      "MemFree:        %llu kB\n"
                      "MemAvailable:   %llu kB\n"
                      "Buffers:        0 kB\n"
                      "Cached:         0 kB\n"
                      "SwapCached:     0 kB\n"
                      "SwapTotal:      0 kB\n"
                      "SwapFree:       0 kB\n",
                      (unsigned long long)total_kb,
                      (unsigned long long)free_kb,
                      (unsigned long long)free_kb);
    *content_len = len > 0 ? (size_t)len : 0;
    return buffer;
}

static char *proc_gen_uptime(size_t *content_len)
{
    uint64_t ns = bootNanoTime();
    uint64_t sec = ns / 1000000000ULL;
    uint64_t cent = (ns % 1000000000ULL) / 10000000ULL;
    char *buffer = (char *)malloc(64);
    int len = sprintf(buffer, "%llu.%02llu %llu.%02llu\n",
                      (unsigned long long)sec,
                      (unsigned long long)cent,
                      (unsigned long long)sec,
                      (unsigned long long)cent);
    *content_len = len > 0 ? (size_t)len : 0;
    return buffer;
}

static char *proc_gen_loadavg(size_t *content_len)
{
    uint64_t runnable = 0;
    uint64_t total = 0;
    if (pcb_group_queue != NULL)
    {
        ProcfsSpinGuard guard(&pcb_group_queue->lock);
        total = pcb_group_queue->size;
        queue_foreach(pcb_group_queue, node)
        {
            pcb_t task = (pcb_t)node->data;
            if (task != NULL && (task->status == RUNNING || task->status == START || task->status == CREATE))
                runnable++;
        }
    }

    char *buffer = (char *)malloc(128);
    int len = sprintf(buffer, "0.00 0.00 0.00 %llu/%llu %llu\n",
                      (unsigned long long)runnable,
                      (unsigned long long)total,
                      (unsigned long long)(now_tid > 0 ? now_tid - 1 : 0));
    *content_len = len > 0 ? (size_t)len : 0;
    return buffer;
}

static char *proc_gen_root_stat(size_t *content_len)
{
    uint64_t procs = pcb_group_queue ? pcb_group_queue->size : 0;
    uint64_t uptime_ticks = bootNanoTime() / 10000000ULL;
    char *buffer = (char *)malloc(PAGE_SIZE);
    int len = sprintf(buffer,
                      "cpu  0 0 0 %llu 0 0 0 0 0 0\n"
                      "cpu0 0 0 0 %llu 0 0 0 0 0 0\n"
                      "intr 0\n"
                      "ctxt 0\n"
                      "btime 0\n"
                      "processes %llu\n"
                      "procs_running 1\n"
                      "procs_blocked 0\n",
                      (unsigned long long)uptime_ticks,
                      (unsigned long long)uptime_ticks,
                      (unsigned long long)procs);
    *content_len = len > 0 ? (size_t)len : 0;
    return buffer;
}

static uint32_t proc_task_uid(pcb_t task)
{
    if (task == NULL) return 0;
    if (task->thread_queue != NULL)
    {
        ProcfsSpinGuard guard(&task->thread_queue->lock);
        queue_foreach(task->thread_queue, node)
        {
            tcb_t thread = (tcb_t)node->data;
            if (thread != NULL && thread->user_info != NULL) return user_uid(thread->user_info);
        }
    }
    return user_uid(&task->permission_user);
}

static uint32_t proc_task_gid(pcb_t task)
{
    if (task == NULL) return 0;
    if (task->thread_queue != NULL)
    {
        ProcfsSpinGuard guard(&task->thread_queue->lock);
        queue_foreach(task->thread_queue, node)
        {
            tcb_t thread = (tcb_t)node->data;
            if (thread != NULL && thread->user_info != NULL) return user_gid(thread->user_info);
        }
    }
    return user_gid(&task->permission_user);
}

static size_t procfs_copy_content(void *addr, size_t offset, size_t size, const char *content, size_t content_len)
{
    if (addr == NULL || content == NULL) return VFS_STATUS_FAILED;
    if (offset >= content_len) return 0;
    size_t to_copy = MIN(size, content_len - offset);
    memcpy(addr, content + offset, to_copy);
    return to_copy;
}

static tcb_t proc_task_first_thread(pcb_t task)
{
    if (task == NULL || task->thread_queue == NULL) return NULL;
    queue_foreach(task->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread != NULL) return thread;
    }
    return NULL;
}

static char proc_task_state(pcb_t task)
{
    if (task == NULL) return 'T';
    switch (task->status)
    {
    case RUNNING: return 'R';
    case WAIT:
    case FUTEX:
    case START:
    case CREATE: return 'S';
    case ZOMBIE: return 'Z';
    case DEATH:
    case OUT:
    default: return 'T';
    }
}

static const char *proc_task_state_text(pcb_t task)
{
    switch (proc_task_state(task))
    {
    case 'R': return "R (running)";
    case 'S': return "S (sleeping)";
    case 'Z': return "Z (zombie)";
    default: return "T (stopped)";
    }
}

static uint64_t proc_thread_signal(pcb_t task)
{
    uint64_t signals = 0;
    if (task == NULL || task->thread_queue == NULL) return 0;
    ProcfsSpinGuard guard(&task->thread_queue->lock);
    queue_foreach(task->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread != NULL) signals |= thread->signal;
    }
    return signals;
}

static uint64_t proc_thread_blocked(pcb_t task)
{
    uint64_t blocked = 0;
    if (task == NULL || task->thread_queue == NULL) return 0;
    ProcfsSpinGuard guard(&task->thread_queue->lock);
    queue_foreach(task->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread != NULL) blocked |= thread->blocked;
    }
    return blocked;
}

static uint64_t proc_task_parent_pid(pcb_t task)
{
    if (!task || !pcb_group_queue) return 0;
    ProcfsSpinGuard guard(&pcb_group_queue->lock);
    return task->parent_task ? task->parent_task->pid : 0;
}

static void proc_task_vma_bounds(pcb_t task, uint64_t *start_code, uint64_t *end_code,
                                 uint64_t *start_data, uint64_t *end_data)
{
    if (start_code) *start_code = 0;
    if (end_code) *end_code = 0;
    if (start_data) *start_data = 0;
    if (end_data) *end_data = 0;
    if (task == NULL) return;

    const uint64_t invalid_addr = ~0ULL;
    uint64_t code_start = invalid_addr;
    uint64_t data_start = invalid_addr;
    uint64_t code_end = 0;
    uint64_t data_end = 0;

    for (vma_t *vma = task->vma_manager.vma_list; vma != NULL; vma = vma->vm_next)
    {
        if (vma->vm_flags & VMA_EXEC)
        {
            code_start = MIN(code_start, (uint64_t)vma->vm_start);
            code_end = MAX(code_end, (uint64_t)vma->vm_end);
        }
        if ((vma->vm_flags & VMA_WRITE) && !(vma->vm_flags & VMA_EXEC))
        {
            data_start = MIN(data_start, (uint64_t)vma->vm_start);
            data_end = MAX(data_end, (uint64_t)vma->vm_end);
        }
    }

    if (code_start == invalid_addr) code_start = task->load_start;
    if (code_end == 0 && task->load_start != 0) code_end = task->load_start + task->elf_size;
    if (data_start == invalid_addr) data_start = task->brk_start;
    if (data_end == 0) data_end = task->brk_current;

    if (start_code) *start_code = code_start;
    if (end_code) *end_code = code_end;
    if (start_data) *start_data = data_start;
    if (end_data) *end_data = data_end;
}

const char *get_vma_permissions(vma_t *vma) {
    static char perms[5];

    perms[0] = (vma->vm_flags & VMA_READ) ? 'r' : '-';
    perms[1] = (vma->vm_flags & VMA_WRITE) ? 'w' : '-';
    perms[2] = (vma->vm_flags & VMA_EXEC) ? 'x' : '-';
    perms[3] = (vma->vm_flags & VMA_SHARED) ? 's' : 'p';
    perms[4] = '\0';

    return perms;
}

static bool procfs_append_map(char *&buffer, size_t &capacity, size_t &offset, const char *text, size_t size)
{
    if (size > size_t(-1) - offset - 1) return false;
    size_t required = offset + size + 1;
    if (required > capacity)
    {
        if (required > size_t(-1) - PAGE_SIZE + 1) return false;
        size_t next_capacity = (required + PAGE_SIZE - 1) & ~(size_t(PAGE_SIZE) - 1);
        char *next = static_cast<char *>(realloc(buffer, next_capacity));
        if (!next) return false;
        buffer = next;
        capacity = next_capacity;
    }
    memcpy(buffer + offset, text, size);
    offset += size;
    buffer[offset] = 0;
    return true;
}

char *proc_gen_maps_file(pcb_t task, size_t *content_len)
{
    *content_len = 0;
    size_t offset = 0, capacity = PAGE_SIZE;
    char *buffer = static_cast<char *>(malloc(capacity));
    if (!buffer) return NULL;
    buffer[0] = 0;
    // The process observation retains collection-owned storage. Concurrent
    // live exec/munmap still requires the separate VMA snapshot protocol.
    for (vma_t *vma = task->vma_manager.vma_list; vma; vma = vma->vm_next)
    {
        FdOperation descriptor;
        uint64_t inode = 0;
        if (vma->vm_fd >= 0 && !descriptor.acquire_process(task, vma->vm_fd))
        {
            vfs_child_lock();
            inode = descriptor.get()->node->inode;
            vfs_child_unlock();
        }
        char permissions[5] = {
            vma->vm_flags & VMA_READ ? 'r' : '-', vma->vm_flags & VMA_WRITE ? 'w' : '-',
            vma->vm_flags & VMA_EXEC ? 'x' : '-', vma->vm_flags & VMA_SHARED ? 's' : 'p', 0
        };
        char line[192];
        int length = snprintf(line, sizeof(line), "%012lx-%012lx %s %08lx %02x:%02x %lu",
                              vma->vm_start, vma->vm_end, permissions, (unsigned long)vma->vm_offset,
                              0, 0, (unsigned long)inode);
        bool valid = length >= 0 && size_t(length) < sizeof(line) &&
                     procfs_append_map(buffer, capacity, offset, line, size_t(length));
        const char *pathname = vma->vm_name;
        if (valid && pathname && *pathname)
            valid = procfs_append_map(buffer, capacity, offset, "               ", 15) &&
                    procfs_append_map(buffer, capacity, offset, pathname, strlen(pathname));
        if (valid) valid = procfs_append_map(buffer, capacity, offset, "\n", 1);
        if (!valid) { free(buffer); return NULL; }
    }
    *content_len = offset;
    return buffer;
}

char *proc_gen_status_file(pcb_t task, size_t *content_len) {
    uint32_t uid = proc_task_uid(task);
    uint32_t gid = proc_task_gid(task);
    uint64_t vsize = task->vma_manager.vm_total;
    if (vsize == 0 && task->brk_current > task->brk_start) vsize = task->brk_current - task->brk_start;
    long rss = (long)((task->vma_manager.vm_used + PAGE_SIZE - 1) / PAGE_SIZE);
    if (rss == 0 && vsize != 0) rss = (long)((vsize + PAGE_SIZE - 1) / PAGE_SIZE);
    char *buffer = (char*)malloc(PAGE_SIZE);
    int len = sprintf(buffer,
                      "Name:\t%s\n"
                      "Umask:\t0022\n"
                      "Pid:\t%llu\n"
                      "PPid:\t%llu\n"
                      "State:\t%s\n"
                      "Tgid:\t%llu\n"
                      "Uid:\t%u\t%u\t%u\t%u\n"
                      "Gid:\t%u\t%u\t%u\t%u\n"
                      "Threads:\t%llu\n"
                      "VmPeak:\t%llu kB\n"
                      "VmSize:\t%llu kB\n",
                      task->name,
                      (unsigned long long)task->pid,
                      (unsigned long long)proc_task_parent_pid(task),
                      proc_task_state_text(task),
                      (unsigned long long)task->pid,
                      uid, uid, uid, uid,
                      gid, gid, gid, gid,
                      (unsigned long long)(task->thread_queue ? task->thread_queue->size : 0),
                      (unsigned long long)(vsize / 1024),
                      (unsigned long long)(vsize / 1024));
    *content_len = len;
    return buffer;
}

char *proc_gen_statm_file(pcb_t task, size_t *content_len)
{
    uint64_t vsize = task->vma_manager.vm_total;
    if (vsize == 0 && task->brk_current > task->brk_start) vsize = task->brk_current - task->brk_start;
    uint64_t size_pages = (vsize + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t resident = (task->vma_manager.vm_used + PAGE_SIZE - 1) / PAGE_SIZE;
    if (resident == 0 && size_pages != 0) resident = size_pages;
    char *buffer = (char *)malloc(128);
    int len = sprintf(buffer, "%llu %llu 0 0 0 0 0\n",
                      (unsigned long long)size_pages,
                      (unsigned long long)resident);
    *content_len = len > 0 ? (size_t)len : 0;
    return buffer;
}

char *proc_gen_stat_file(pcb_t task, size_t *content_len) {
    tcb_t main_thread = proc_task_first_thread(task);
    uint64_t start_code = 0;
    uint64_t end_code = 0;
    uint64_t start_data = 0;
    uint64_t end_data = 0;
    proc_task_vma_bounds(task, &start_code, &end_code, &start_data, &end_data);

    uint64_t signal_pending = proc_thread_signal(task);
    uint64_t blocked = proc_thread_blocked(task);
    uint64_t sigignore = 0;
    uint64_t sigcatch = 0;
    if (main_thread != NULL)
    {
        for (size_t i = 0; i < MAX_SIGNALS; i++)
        {
            if (main_thread->actions[i].sa_handler == SIG_IGN) sigignore |= (1ULL << i);
            else if (main_thread->actions[i].sa_handler != SIG_DFL &&
                     main_thread->actions[i].sa_handler != NULL)
            {
                sigcatch |= (1ULL << i);
            }
        }
    }

    uint64_t vsize = task->vma_manager.vm_total;
    if (vsize == 0 && task->brk_current > task->brk_start) vsize = task->brk_current - task->brk_start;
    long rss = (long)((task->vma_manager.vm_used + PAGE_SIZE - 1) / PAGE_SIZE);
    if (rss == 0 && vsize != 0) rss = (long)((vsize + PAGE_SIZE - 1) / PAGE_SIZE);

    uint64_t start_stack = main_thread ? main_thread->user_stack : 0;
    uint64_t kstkesp = main_thread ? main_thread->kernel_stack : 0;
    uint64_t ksteip = main_thread ? main_thread->context0.rip : 0;
    uint32_t processor = main_thread ? (uint32_t)main_thread->cpu_id : 0;
    uint32_t flags = task->linux_abi ? 0x00400000u : 0;
    int exit_signal = task->status == ZOMBIE ? SIGCHLD : 0;
    int priority = task->task_level == TASK_KERNEL_LEVEL ? 5 : 6;
    size_t num_threads = task->thread_queue ? task->thread_queue->size : 0;

    char *buffer = (char*)malloc(PAGE_SIZE * 4);
    int   len    = sprintf(buffer,
                           "%d (%s) %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld "
                                "%ld %ld %llu %lu %ld %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu "
                                "%lu %d %d %u %u %llu %lu %ld %lu %lu %lu %lu %lu %lu %lu %d\n",
                           (int)task->pid,  // pid
                           task->name, // name
                           proc_task_state(task),                        // state
                           (int)proc_task_parent_pid(task), // ppid
                           (int)task->pid,                              // pgrp
                           (int)(proc_task_parent_pid(task) ? proc_task_parent_pid(task) : task->pid), // session
                           0,                                            // tty_nr
                           (int)task->pid,                              // tpgid
                           flags,                                        // flags
                           0,                                            // minflt
                           0,                                            // cminflt
                           0,                                            // majflt
                           0,                                            // cmajflt
                           0,                                            // utime
                           0,                                            // stime
                           0,                                            // cutime
                           0,                                            // cstime
                           priority,                                     // priority
                           0,                                            // nice
                           num_threads,                                  // num_threads
                           0,                                            // itrealvalue
                           0,                                            // starttime
                           vsize,                                        // vsize
                           rss,                                          // rss
                           0,                                            // rsslim
                           start_code,                                   // startcode
                           end_code,                                     // endcode
                           start_stack,                                  // startstack
                           kstkesp,                                      // kstkesp
                           ksteip,                                       // ksteip
                           signal_pending,                               // signal
                           blocked,                                      // blocked
                           sigignore,                                    // sigignore
                           sigcatch,                                     // sigcatch
                           0,                                            // wchan
                           0,                                            // nswap
                           0,                                            // cnswap
                           exit_signal,                                  // exit_signal
                           processor,                                    // processor
                           0,                                            // rt_priority
                           0,                                            // policy
                           0,                                            // delayacct_blkio_ticks
                           0,                                            // guest_time
                           0,                                            // cguest_time
                           start_data,                                   // start_data
                           end_data,                                     // end_data
                           task->brk_start,                              // start_brk
                           0,                                            // arg_start
                           0,                                            // arg_end
                           0,                                            // env_start
                           0,                                            // env_end
                           task->exit_code                               // exit_code
         );

    *content_len = len;

    return buffer;
}

char *proc_gen_mounts(size_t *context_len) {
    char *mount_info =
        "dev /dev devfs rw,nosuid,relatime,mode=755,inode64 0 0\n"
        "proc /proc proc ro,nosuid,nodev,noexec,relatime 0 0\n"
        "tmpfs /tmp tmpfs rw,nosuid,size=8040232k,nr_inodes=1048576,nodev,inode64,usrquota 0 0";
    *context_len = strlen(mount_info);
    return strdup(mount_info);
}

static const char *cpu_feature_name(uint32_t bit, bool in_edx)
{
    static const char *edx_names[] = {
        [0]="fpu", [1]="vme", [2]="de", [3]="pse", [4]="tsc", [5]="msr",
        [6]="pae", [7]="mce", [8]="cx8", [9]="apic", [11]="sep",
        [12]="mtrr", [13]="pge", [14]="mca", [15]="cmov", [16]="pat",
        [17]="pse36", [19]="clflush", [23]="mmx", [24]="fxsr",
        [25]="sse", [26]="sse2", [28]="ht", [31]="pbe"
    };
    static const char *ecx_names[] = {
        [0]="pni", [1]="pclmulqdq", [9]="ssse3", [12]="fma",
        [13]="cx16", [19]="sse4_1", [20]="sse4_2", [22]="movbe",
        [23]="popcnt", [25]="aes", [26]="xsave", [27]="osxsave",
        [28]="avx", [29]="f16c", [30]="rdrand"
    };
    if (in_edx) {
        if (bit < 32 && edx_names[bit]) return edx_names[bit];
    } else {
        if (bit < 32 && ecx_names[bit]) return ecx_names[bit];
    }
    return NULL;
}

static void append_flags(char *buf, size_t *pos, size_t buf_size, uint32_t edx, uint32_t ecx)
{
    static const char *extra[] = {"syscall", "nx", "pdpe1gb", "rdtscp", "lm",
                                  "constant_tsc", "rep_good", "nopl", "xtopology",
                                  "cpuid", "tsc_known_freq", "hypervisor", "lahf_lm",
                                  "abm", "3dnowprefetch", "fsgsbase", "bmi1", "avx2",
                                  "smep", "bmi2", "erms", "invpcid", "rdseed", "adx",
                                  "smap", "clflushopt", "sha_ni", NULL};
    for (uint32_t i = 0; i < 32; i++) {
        if (edx & (1U << i)) {
            const char *name = cpu_feature_name(i, true);
            if (name) { *pos += snprintf(buf + *pos, buf_size - *pos, "%s ", name); }
        }
    }
    for (uint32_t i = 0; i < 32; i++) {
        if (ecx & (1U << i)) {
            const char *name = cpu_feature_name(i, false);
            if (name) { *pos += snprintf(buf + *pos, buf_size - *pos, "%s ", name); }
        }
    }
    for (int i = 0; extra[i]; i++) {
        *pos += snprintf(buf + *pos, buf_size - *pos, "%s ", extra[i]);
    }
}

static uint32_t cpu_cache_size_kb(uint32_t max_basic)
{
    (void)max_basic;
    uint32_t max_ext = 0, ebx = 0, ecx = 0, edx = 0;
    asm_cpuid(0x80000000, 0, &max_ext, &ebx, &ecx, &edx);
    if (max_ext >= 0x80000006)
    {
        uint32_t eax6 = 0, ebx6 = 0, ecx6 = 0, edx6 = 0;
        asm_cpuid(0x80000006, 0, &eax6, &ebx6, &ecx6, &edx6);
        uint32_t l2_kb = (ecx6 >> 16) & 0xFFFF;
        if (l2_kb != 0) return l2_kb;
    }
    return 0;
}

static inline uint64_t proc_rdtsc()
{
    uint32_t low = 0, high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static uint64_t cpu_mhz_sample()
{
    static uint64_t cached_mhz = 0;
    if (cached_mhz != 0) return cached_mhz;

    uint64_t start_ns = nanoTime();
    if (start_ns == 0) return 0;

    uint64_t start_tsc = proc_rdtsc();
    uint64_t elapsed_ns = 0;
    do {
        elapsed_ns = nanoTime() - start_ns;
    } while (elapsed_ns < 10000000ULL);

    uint64_t elapsed_cycles = proc_rdtsc() - start_tsc;
    cached_mhz = (elapsed_cycles + elapsed_ns / 2) / elapsed_ns;
    return cached_mhz;
}

static uint64_t cpu_mhz_from_cpuid(uint32_t max_basic)
{
    if (max_basic >= 0x16)
    {
        uint32_t base_mhz = 0, max_mhz = 0, bus_mhz = 0, edx = 0;
        asm_cpuid(0x16, 0, &base_mhz, &max_mhz, &bus_mhz, &edx);
        if (base_mhz != 0) return base_mhz;
    }

    if (max_basic >= 0x15)
    {
        uint32_t denom = 0, numer = 0, crystal_hz = 0, edx = 0;
        asm_cpuid(0x15, 0, &denom, &numer, &crystal_hz, &edx);
        if (denom != 0 && numer != 0 && crystal_hz != 0)
        {
            uint64_t tsc_hz = ((uint64_t)crystal_hz * numer) / denom;
            return (tsc_hz + 500000ULL) / 1000000ULL;
        }
    }

    return 0;
}

char *proc_gen_cpuinfo(size_t *content_len)
{
    size_t ncpus = xsi ? xsi->cpu_count : 1;
    size_t buf_size = ncpus * 2048 + 256;
    char *buf = (char *)malloc(buf_size);
    if (!buf) { *content_len = 0; return NULL; }
    size_t pos = 0;

    char vendor[13];
    get_cpu_vendor(vendor);

    uint32_t eax0, ebx0, ecx0, edx0;
    asm_cpuid(0, 0, &eax0, &ebx0, &ecx0, &edx0);
    uint32_t max_basic = eax0;

    uint32_t eax1, ebx1, ecx1, edx1;
    asm_cpuid(1, 0, &eax1, &ebx1, &ecx1, &edx1);
    uint32_t family   = (eax1 >> 8) & 0xF;
    uint32_t model    = (eax1 >> 4) & 0xF;
    uint32_t stepping = eax1 & 0xF;
    if (family == 0xF) family += (eax1 >> 20) & 0xFF;
    if (family >= 0x6) model |= ((eax1 >> 16) & 0xF) << 4;

    uint32_t apic_id = (ebx1 >> 24) & 0xFF;
    uint32_t clflush_size = ((ebx1 >> 8) & 0xFF) * 8;
    uint32_t cache_size_kb = cpu_cache_size_kb(max_basic);
    uint64_t cpu_mhz = cpu_mhz_from_cpuid(max_basic);
    if (cpu_mhz == 0) cpu_mhz = cpu_mhz_sample();
    uint64_t bogomips_int = cpu_mhz * 2;

    char model_name[48];
    get_cpu_name(model_name);

    for (size_t cpu = 0; cpu < ncpus; cpu++) {
        uint32_t lapic_id = apic_id + (uint32_t)cpu;
        pos += snprintf(buf + pos, buf_size - pos,
            "processor\t: %lu\n"
            "vendor_id\t: %s\n"
            "cpu family\t: %u\n"
            "model\t\t: %u\n"
            "model name\t: %s\n"
            "stepping\t: %u\n"
            "microcode\t: 0x0\n"
            "cpu MHz\t\t: %llu.000\n"
            "cache size\t: %u KB\n"
            "physical id\t: %lu\n"
            "siblings\t: %lu\n"
            "core id\t\t: %lu\n"
            "cpu cores\t: %lu\n"
            "apicid\t\t: %u\n"
            "initial apicid\t: %u\n"
            "fpu\t\t: yes\n"
            "fpu_exception\t: yes\n"
            "cpuid level\t: %u\n"
            "wp\t\t: yes\n"
            "flags\t\t: ",
            cpu, vendor, family, model, model_name, stepping,
            cpu_mhz, cache_size_kb, cpu, ncpus, cpu, ncpus, lapic_id, lapic_id, max_basic);
        append_flags(buf, &pos, buf_size, edx1, ecx1);
        pos += snprintf(buf + pos, buf_size - pos,
            "\nbogomips\t: %llu.00\n"
            "clflush size\t: %u\n"
            "cache_alignment\t: 64\n"
            "address sizes\t: 48 bits physical, 48 bits virtual\n"
            "power management:\n\n",
            bogomips_int, clflush_size);
    }
    *content_len = pos;
    return buf;
}

static void procfs_discard_private(vfs_node_t node);

static vfs_node_t procfs_prepare_global(vfs_node_t parent, const char *name, uint16_t type,
                                       uint16_t mode, const char *kind)
{
    auto *handle = kind ? static_cast<proc_handle_t *>(calloc(1, sizeof(proc_handle_t))) : NULL;
    if (kind && !handle) return NULL;
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (!node) { free(handle); return NULL; }
    node->type = type;
    node->mode = mode;
    node->owner = node->group = 0;
    node->fsid = procfs_id;
    if (kind)
    {
        handle->node = node;
        strcpy(handle->name, kind);
        node->handle = handle;
    }
    return node;
}

static void procfs_rebind_private(vfs_node_t node, vfs_node_t mount)
{
    node->root = mount;
    node->dev = mount->dev;
    for (list_t entry = node->child; entry; entry = entry->next)
        procfs_rebind_private(static_cast<vfs_node_t>(entry->data), mount);
}

errno_t procfs_mount(const char *src, vfs_node_t node)
{
    // This global singleton is initialized only by procfs_setup while the boot
    // scheduler gate is closed. sys_mount can translate strings to this token,
    // so the token alone is not authorization or an exclusivity guarantee.
    tcb_t owner = get_current_task();
    if (src != (void *)PROC_REGISTER_ID || !owner || owner != procfs_bootstrap_owner ||
        owner->tid != procfs_bootstrap_tid || scheduler_is_enabled()) return -EOPNOTSUPP;
    if (procfs_root) return -EBUSY;
    if (!node) return -EINVAL;
    vfs_node_t prepared = procfs_prepare_global(NULL, node->name, file_dir, 0555, "proc_root");
    if (!prepared) return -ENOMEM;
    bool ready = true;
    vfs_node_t self = procfs_prepare_global(prepared, "self", file_symlink, 0644, NULL);
    if (!self) ready = false;
    else
    {
        auto *handle = static_cast<procfs_self_handle_t *>(calloc(1, sizeof(procfs_self_handle_t)));
        if (!handle) ready = false;
        else
        {
            handle->self = self;
            self->handle = handle;
            self->fsid = proc_self_id;
        }
    }
    struct GlobalFile { const char *name; const char *kind; } root_files[] = {
        {"filesystems", "filesystems"}, {"cpuinfo", "cpuinfo"}, {"cmdline", "cmdline"},
        {"meminfo", "meminfo"}, {"uptime", "uptime"}, {"loadavg", "loadavg"},
        {"stat", "stat"}, {"mounts", "mounts"}, {"version", "version"},
    };
    for (const GlobalFile &file : root_files)
        if (ready && !procfs_prepare_global(prepared, file.name, file_none, 0444, file.kind)) ready = false;
    vfs_node_t sys = ready ? procfs_prepare_global(prepared, "sys", file_dir, 0555, NULL) : NULL;
    vfs_node_t kernel = sys ? procfs_prepare_global(sys, "kernel", file_dir, 0555, NULL) : NULL;
    if (!kernel) ready = false;
    GlobalFile kernel_files[] = {
        {"ostype", "sys_kernel_ostype"}, {"osrelease", "sys_kernel_osrelease"},
        {"version", "sys_kernel_version"}, {"hostname", "sys_kernel_hostname"},
    };
    for (const GlobalFile &file : kernel_files)
        if (ready && !procfs_prepare_global(kernel, file.name, file_none, 0444, file.kind)) ready = false;
    if (!ready) { procfs_discard_private(prepared); return -ENOMEM; }
    procfs_rebind_private(prepared, node);
    for (list_t entry = prepared->child; entry; entry = entry->next)
        static_cast<vfs_node_t>(entry->data)->parent = node;
    static_cast<proc_handle_t *>(prepared->handle)->node = node;
    // Before changing fsid, finalize the old mountpoint using its own backend.
    // The internal bootstrap-owner scope remains exclusive through publication.
    int error = vfs_close_cached_backend(node);
    if (error) { procfs_discard_private(prepared); return error; }
    vfs_child_lock();
    node->handle = prepared->handle;
    node->child = prepared->child;
    node->fsid = procfs_id;
    node->root = node;
    node->is_mount = true;
    node->type = file_dir;
    node->size = 0;
    node->owner = node->group = 0;
    node->mode = 0555;
    prepared->handle = NULL;
    prepared->child = NULL;
    procfs_root = node;
    vfs_child_unlock();
    vfs_free(prepared);
    return 0;
}

void procfs_open(void *parent, const char *name, vfs_node_t node)
{
    UNUSED(parent, name);
    if (node == procfs_root) procfs_update_task_list();
    auto *handle = static_cast<proc_handle_t *>(node->handle);
    if (handle && handle->process && !strcmp(handle->name, "proc_fd_dir"))
    {
        ProcfsOperation operation(handle->process);
        if (operation.get()) procfs_refresh_fd_dir(node, operation);
        else permissions_set_file_error(ENOENT);
    }
}

void procfs_close(void *current)
{
    auto *handle = static_cast<proc_handle_t *>(current);
    if (!handle) return;
    ProcfsProcessContext *process = handle->process;
    free(handle);
    procfs_context_release(process);
}

size_t procfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    if (!node || !addr || !node->handle) return VFS_STATUS_FAILED;
    auto *handle = static_cast<proc_handle_t *>(node->handle);
    ProcfsOperation operation(handle->process);
    pcb_t task = operation.get();
    if (handle->process && !task)
    {
        permissions_set_file_error(ENOENT);
        return VFS_STATUS_FAILED;
    }
    if (!strcmp(handle->name, "proc_exe"))
    {
        const char *target = task && task->exe_path ? task->exe_path : "";
        return procfs_copy_content(addr, offset, size, target, strlen(target));
    }
    if (!strcmp(handle->name, "proc_fd"))
    {
        size_t number;
        if (!task || !procfs_fd_number(node->name, number)) return VFS_STATUS_FAILED;
        // Declare the FD ticket after the process ticket: release the
        // descriptor before dropping the PCB operation pin on every return.
        FdOperation descriptor;
        int error = descriptor.acquire_process(task, int(number));
        if (error) { permissions_set_file_error(-error); return VFS_STATUS_FAILED; }
        vfs_node_t target_node = descriptor.get()->node;
        char special[64];
        const char *target = NULL;
        vfs_child_lock();
        if (target_node->type & file_pipe)
        {
            snprintf(special, sizeof(special), "pipe:[%llu]", (unsigned long long)target_node->inode);
            target = special;
        }
        else if (target_node->type & file_socket)
        {
            snprintf(special, sizeof(special), "socket:[%llu]", (unsigned long long)target_node->inode);
            target = special;
        }
        vfs_child_unlock();
        char *path = target ? NULL : vfs_get_fullpath(target_node);
        if (!target) target = path;
        size_t result = target ? procfs_copy_content(addr, offset, size, target, strlen(target)) : VFS_STATUS_FAILED;
        free(path);
        return result;
    }
    return VFS_STATUS_FAILED;
}

size_t procfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    UNUSED(addr, offset, size);
    auto *handle = static_cast<proc_handle_t *>(file);
    if (!handle) return VFS_STATUS_FAILED;
    ProcfsOperation operation(handle->process);
    if (handle->process && !operation.get())
    {
        permissions_set_file_error(ENOENT);
        return VFS_STATUS_FAILED;
    }
    permissions_set_file_error(EROFS);
    return VFS_STATUS_FAILED;
}

size_t procfs_read(void *file, void *addr, size_t offset, size_t size) {
    proc_handle_t *handle = (proc_handle_t *)file;
    if (!handle) { return VFS_STATUS_FAILED; }
    ProcfsOperation operation(handle->process);
    pcb_t task = operation.get();
    if (handle->process && !task)
    {
        permissions_set_file_error(ENOENT);
        return VFS_STATUS_FAILED;
    }
    if (!handle->process && get_current_task()) task = get_current_task()->parent_group;

    if (!strcmp(handle->name, "filesystems")) {
        size_t fs_size = strlen(filesystems_content);
        return procfs_copy_content(addr, offset, size, filesystems_content, fs_size);
    } else if (!strcmp(handle->name, "cpuinfo")) {
        size_t len = 0;
        char *content = proc_gen_cpuinfo(&len);
        if (!content) return VFS_STATUS_FAILED;
        size_t copied = procfs_copy_content(addr, offset, size, content, len);
        free(content);
        return copied;
    } else if (!strcmp(handle->name, "cmdline")) {
        size_t len = strlen(procfs_kernel_cmdline());
        return procfs_copy_content(addr, offset, size, procfs_kernel_cmdline(), len);
    } else if (!strcmp(handle->name, "meminfo")) {
        size_t len = 0;
        char *content = proc_gen_meminfo(&len);
        size_t copied = procfs_copy_content(addr, offset, size, content, len);
        free(content);
        return copied;
    } else if (!strcmp(handle->name, "uptime")) {
        size_t len = 0;
        char *content = proc_gen_uptime(&len);
        size_t copied = procfs_copy_content(addr, offset, size, content, len);
        free(content);
        return copied;
    } else if (!strcmp(handle->name, "loadavg")) {
        size_t len = 0;
        char *content = proc_gen_loadavg(&len);
        size_t copied = procfs_copy_content(addr, offset, size, content, len);
        free(content);
        return copied;
    } else if (!strcmp(handle->name, "stat")) {
        size_t len = 0;
        char *content = proc_gen_root_stat(&len);
        size_t copied = procfs_copy_content(addr, offset, size, content, len);
        free(content);
        return copied;
    } else if (!strcmp(handle->name, "version")) {
        size_t len = strlen(procfs_kernel_version());
        return procfs_copy_content(addr, offset, size, procfs_kernel_version(), len);
    } else if (!strcmp(handle->name, "sys_kernel_ostype")) {
        size_t len = strlen(procfs_kernel_ostype());
        return procfs_copy_content(addr, offset, size, procfs_kernel_ostype(), len);
    } else if (!strcmp(handle->name, "sys_kernel_osrelease")) {
        size_t len = strlen(procfs_kernel_osrelease());
        return procfs_copy_content(addr, offset, size, procfs_kernel_osrelease(), len);
    } else if (!strcmp(handle->name, "sys_kernel_version")) {
        size_t len = strlen(procfs_kernel_version());
        return procfs_copy_content(addr, offset, size, procfs_kernel_version(), len);
    } else if (!strcmp(handle->name, "sys_kernel_hostname")) {
        size_t len = strlen(procfs_kernel_hostname());
        return procfs_copy_content(addr, offset, size, procfs_kernel_hostname(), len);
    } else if (!strcmp(handle->name, "mounts")) {
        size_t len     = 0;
        char  *contect = proc_gen_mounts(&len);
        size_t copied = procfs_copy_content(addr, offset, size, contect, len);
        free(contect);
        return copied;
    } else if (!strcmp(handle->name, "proc_cmdline")) {
        return procfs_copy_task_cmdline(task, addr, offset, size);
    } else if (!strcmp(handle->name, "proc_comm")) {
        char comm[sizeof(task->name) + 2];
        int len = sprintf(comm, "%s\n", task->name);
        return procfs_copy_content(addr, offset, size, comm, len > 0 ? (size_t)len : 0);
    } else if (!strcmp(handle->name, "proc_statm")) {
        size_t content_len = 0;
        char *content = proc_gen_statm_file(task, &content_len);
        size_t to_copy = procfs_copy_content(addr, offset, size, content, content_len);
        free(content);
        return to_copy;
    } else if (!strcmp(handle->name, "proc_maps")) {
        size_t content_len = 0;
        char  *content     = proc_gen_maps_file(task, &content_len);
        size_t to_copy = procfs_copy_content(addr, offset, size, content, content_len);
        free(content);
        return to_copy;
    } else if (!strcmp(handle->name, "dri_name")) {
        char name[] = "cpkernel_drm";
        int  len    = strlen(name);
        return procfs_copy_content(addr, offset, size, name, len);
    } else if (!strcmp(handle->name, "proc_stat")) {
        size_t content_len = 0;
        char  *content     = proc_gen_stat_file(task, &content_len);
        size_t to_copy = procfs_copy_content(addr, offset, size, content, content_len);
        free(content);
        return to_copy;
    } else if (!strcmp(handle->name, "proc_status")) {
        size_t content_len = 0;
        char  *content     = proc_gen_status_file(task, &content_len);
        size_t to_copy = procfs_copy_content(addr, offset, size, content, content_len);
        free(content);
        return to_copy;
    } else if (!strcmp(handle->name, "proc_exe")) {
        const char *target = task && task->exe_path ? task->exe_path : "";
        return procfs_copy_content(addr, offset, size, target, strlen(target));
    }
    return VFS_STATUS_FAILED;
}

vfs_node_t procfs_dup(vfs_node_t src) {
    return src;
}

errno_t procfs_stat(void *file, vfs_node_t node) {
    if (file == NULL) return EOK;
    proc_handle_t *handle = (proc_handle_t *)file;
    if (node == procfs_root)
    {
        procfs_update_task_list();
        node->size = 0;
        return EOK;
    }
    ProcfsOperation operation(handle->process);
    pcb_t task = operation.get();
    if (handle->process && !task)
    {
        permissions_set_file_error(ENOENT);
        return -ENOENT;
    }
    if (!strcmp(handle->name, "filesystems"))
        node->size = strlen(filesystems_content);
    else if (!strcmp(handle->name, "cpuinfo")) {
        size_t content_len = 0;
        char *content = proc_gen_cpuinfo(&content_len);
        free(content);
        node->size = content_len;
    } else if (!strcmp(handle->name, "cmdline"))
        node->size = strlen(procfs_kernel_cmdline());
    else if (!strcmp(handle->name, "meminfo")) {
        size_t content_len = 0;
        char *content = proc_gen_meminfo(&content_len);
        free(content);
        node->size = content_len;
    } else if (!strcmp(handle->name, "uptime")) {
        size_t content_len = 0;
        char *content = proc_gen_uptime(&content_len);
        free(content);
        node->size = content_len;
    } else if (!strcmp(handle->name, "loadavg")) {
        size_t content_len = 0;
        char *content = proc_gen_loadavg(&content_len);
        free(content);
        node->size = content_len;
    } else if (!strcmp(handle->name, "stat")) {
        size_t content_len = 0;
        char *content = proc_gen_root_stat(&content_len);
        free(content);
        node->size = content_len;
    } else if (!strcmp(handle->name, "version")) {
        node->size = strlen(procfs_kernel_version());
    } else if (!strcmp(handle->name, "sys_kernel_ostype")) {
        node->size = strlen(procfs_kernel_ostype());
    } else if (!strcmp(handle->name, "sys_kernel_osrelease")) {
        node->size = strlen(procfs_kernel_osrelease());
    } else if (!strcmp(handle->name, "sys_kernel_version")) {
        node->size = strlen(procfs_kernel_version());
    } else if (!strcmp(handle->name, "sys_kernel_hostname")) {
        node->size = strlen(procfs_kernel_hostname());
    }
    else if (!strcmp(handle->name, "proc_cmdline"))
        node->size = procfs_task_cmdline_len(task);
    else if (!strcmp(handle->name, "proc_comm"))
        node->size = strlen(task->name) + 1;
    else if (!strcmp(handle->name, "proc_exe"))
        node->size = task && task->exe_path ? strlen(task->exe_path) : 0;
    else if (!strcmp(handle->name, "proc_statm")) {
        size_t content_len = 0;
        char *content = proc_gen_statm_file(task, &content_len);
        free(content);
        node->size = content_len;
    }
    else if (!strcmp(handle->name, "proc_maps")) {
        size_t content_len = 0;
        char  *content     = proc_gen_maps_file(task, &content_len);
        free(content);
        node->size = content_len;
    } else if (!strcmp(handle->name, "dri_name"))
        node->size = strlen("cpkernel_drm");
    else if (!strcmp(handle->name, "proc_fd_dir"))
    {
        procfs_refresh_fd_dir(node, operation);
        node->size = 0;
    }
    else if (!strcmp(handle->name, "proc_fd"))
        node->size = 64;
    else if (!strcmp(handle->name, "proc_stat")) {
        size_t content_len = 0;
        char  *content     = proc_gen_stat_file(task, &content_len);
        node->size         = content_len;
        free(content);
    } else if (!strcmp(handle->name, "proc_status")) {
        size_t content_len = 0;
        char  *content     = proc_gen_status_file(task, &content_len);
        node->size         = content_len;
        free(content);
    } else if (!strcmp(handle->name, "mounts")) {
        size_t content_len = 0;
        char  *content     = proc_gen_mounts(&content_len);
        free(content);
        node->size = content_len;
    }
    return EOK;
}

void procfs_self_open(void *parent, const char *name, vfs_node_t node) {
    UNUSED(parent, name);
    procfs_self_handle_t *handle = (procfs_self_handle_t *)malloc(sizeof(procfs_self_handle_t));
    if (!handle) { permissions_set_file_error(ENOMEM); return; }
    handle->self                 = node;
    node->handle                 = handle;
}

void procfs_self_close(void *current) {
    procfs_self_handle_t *handle  = (procfs_self_handle_t *)current;
    if (handle != NULL && handle->self != NULL)
    {
        handle->self->handle = NULL;
        handle->self->linkto = NULL;
    }
    free(handle);
}

size_t procfs_self_read(void *fd, void *addr, size_t offset, size_t size) {
    procfs_self_handle_t *handle = (procfs_self_handle_t *)fd;
    vfs_node_t target = procfs_current_self_target();
    if (handle == NULL || handle->self == NULL || target == NULL || target->handle == NULL) return VFS_STATUS_FAILED;
    return procfs_read(target->handle, addr, offset, size);
}

size_t procfs_self_write(void *fd, const void *addr, size_t offset, size_t size) {
    procfs_self_handle_t *handle = (procfs_self_handle_t *)fd;
    vfs_node_t target = procfs_current_self_target();
    if (handle == NULL || handle->self == NULL || target == NULL || target->handle == NULL) return VFS_STATUS_FAILED;
    return procfs_write(target->handle, addr, offset, size);
}

size_t procfs_self_readlink(vfs_node_t file, void *addr, size_t offset, size_t size) {
    procfs_self_handle_t *handle = (procfs_self_handle_t *)file->handle;
    vfs_node_t target_node = procfs_current_self_target();
    if (handle == NULL || handle->self == NULL || target_node == NULL || target_node->name == NULL)
        return VFS_STATUS_FAILED;
    file->linkto = NULL;
    char target_path[MAX_PID_NAME_LEN + 8];
    sprintf(target_path, "/proc/%s", target_node->name);
    return procfs_copy_content(addr, offset, size, target_path, strlen(target_path));
}

errno_t procfs_self_stat(void *file, vfs_node_t node) {
    procfs_self_handle_t *handle  = (procfs_self_handle_t *)file;
    vfs_node_t target             = procfs_current_self_target();
    node->type                   |= file_symlink;
    node->linkto                  = NULL;
    node->size                    = handle && handle->self && target && target->name ?
                                    strlen(target->name) + strlen("/proc/") : 0;
    return EOK;
}

void procfs_self_free_handle(procfs_self_handle_t *handle) {
    free(handle);
}

static struct vfs_callback procfs_self_callbacks = {
    .mount    = (vfs_mount_t)udummy,
    .unmount  = (vfs_unmount_t)dummy,
    .open     = procfs_self_open,
    .close    = procfs_self_close,
    .read     = procfs_self_read,
    .write    = procfs_self_write,
    .readlink = procfs_self_readlink,
    .mkdir    = (vfs_mk_t)dummy,
    .mkfile   = (vfs_mk_t)dummy,
    .link     = (vfs_mk_t)dummy,
    .symlink  = (vfs_mk_t)dummy,
    .stat     = procfs_self_stat,
    .ioctl    = (vfs_ioctl_t)dummy,
    .dup      = (vfs_dup_t)dummy,
    .poll     = (vfs_poll_t)dummy,
    .map      = (vfs_mapfile_t)dummy,
    .resize   = (vfs_resize_t)dummy,
    .del      = (vfs_del_t)dummy,
    .rename   = (vfs_rename_t)dummy,
};

static struct vfs_callback procfs_callbacks = {
    .mount    = procfs_mount,
    .unmount  = (vfs_unmount_t)dummy,
    .open     = procfs_open,
    .close    = procfs_close,
    .read     = procfs_read,
    .write    = procfs_write,
    .readlink = procfs_readlink,
    .mkdir    = (vfs_mk_t)dummy,
    .mkfile   = (vfs_mk_t)dummy,
    .link     = (vfs_mk_t)dummy,
    .symlink  = (vfs_mk_t)dummy,
    .stat     = procfs_stat,
    .ioctl    = (vfs_ioctl_t)dummy,
    .dup      = procfs_dup,
    .poll     = (vfs_poll_t)dummy,
    .map      = (vfs_mapfile_t)dummy,
    .resize   = (vfs_resize_t)dummy,
    .del      = (vfs_del_t)dummy,
    .rename   = (vfs_rename_t)dummy,
};

void procfs_setup()
{
    if (procfs_root || !get_current_task() || scheduler_is_enabled()) return;
    procfs_id = vfs_regist("procfs", &procfs_callbacks, PROC_REGISTER_ID, 0x9fa0);
    proc_self_id = vfs_regist("proc_self", &procfs_self_callbacks, PROC_REGISTER_ID, 0x0);
    if (procfs_id == VFS_STATUS_FAILED || proc_self_id == VFS_STATUS_FAILED)
    {
        write_serial_fmt("procfs register error\n");
        return;
    }
    vfs_set_unmount_quiescence(procfs_id, true);
    // All current proc entries are observation interfaces. Dummy mutation
    // callbacks must not let a privileged caller unlink/rename context nodes.
    vfs_set_readonly(procfs_id, true);
    vfs_set_readonly(proc_self_id, true);
    vfs_mkdir("/proc");
    vfs_node_t node = vfs_open("/proc");
    if (!node) { write_serial_fmt("procfs mountpoint unavailable\n"); return; }
    procfs_bootstrap_owner = get_current_task();
    procfs_bootstrap_tid = procfs_bootstrap_owner->tid;
    int error = procfs_mount((const char *)PROC_REGISTER_ID, node);
    procfs_bootstrap_owner = NULL;
    procfs_bootstrap_tid = 0;
    vfs_close(node);
    if (error) write_serial_fmt("procfs bootstrap mount failed (%d)\n", error);
}

// All process-cache publication happens in ordinary task context. The legacy
// creation hook intentionally does no VFS work while the creator spin lock and
// per-CPU scheduler gate are held; the published process list is authoritative.
void procfs_on_new_task(pcb_t task)
{
    UNUSED(task);
}

static vfs_node_t procfs_private_process_node(vfs_node_t parent, const char *name, uint16_t type,
                                             uint16_t mode, ProcfsProcessContext *process,
                                             uint32_t uid, uint32_t gid, const char *kind)
{
    auto *handle = static_cast<proc_handle_t *>(calloc(1, sizeof(proc_handle_t)));
    if (!handle || !procfs_context_retain(process)) { free(handle); return NULL; }
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (!node)
    {
        procfs_context_release(process);
        free(handle);
        return NULL;
    }
    node->type = type;
    node->mode = mode;
    node->owner = uid;
    node->group = gid;
    node->fsid = procfs_id;
    node->root = procfs_root->root;
    node->dev = procfs_root->dev;
    handle->node = node;
    handle->process = process;
    strcpy(handle->name, kind);
    node->handle = handle;
    return node;
}

static void procfs_discard_private(vfs_node_t node)
{
    if (!node) return;
    // Unlike a generic directory reference close, the checked disposer closes
    // directory backend handles too. All nodes here are unpublished/refquiet
    // and procfs close cannot fail or issue storage I/O.
    int error = vfs_try_free_detached(node);
    if (error) write_serial_fmt("procfs: retaining failed private cache disposal (%d)\n", error);
}

static vfs_node_t procfs_ensure_process(ProcfsOperation &operation)
{
    ProcfsProcessContext *process = operation.context();
    pcb_t task = operation.get();
    if (!process || !task || !procfs_root) return NULL;
    uint32_t uid = proc_task_uid(task), gid = proc_task_gid(task);
    {
        ProcfsSpinGuard guard(&process_context_lock);
        if (process->retired) return NULL;
        if (process->node)
        {
            // The immutable PID namespace has eight fixed direct children.
            // FD entries refresh their ownership separately, one name at a
            // time, so an unbounded FD list is never walked under this lock.
            vfs_child_lock();
            process->node->owner = uid;
            process->node->group = gid;
            for (list_t entry = process->node->child; entry; entry = entry->next)
            {
                auto *node = static_cast<vfs_node_t>(entry->data);
                if (!vfs_node_retiring_locked(node))
                {
                    node->owner = uid;
                    node->group = gid;
                }
            }
            vfs_child_unlock();
            return process->node;
        }
        vfs_child_lock();
        vfs_node_t mount = procfs_root->root;
        bool needs_pin = mount && mount != rootdir && mount->is_mount && !process->mount_pin;
        bool busy = vfs_node_retiring_locked(procfs_root) || (needs_pin && mount->refcount == uint32_t(-1));
        if (!busy && needs_pin) { ++mount->refcount; process->mount_pin = mount; }
        vfs_child_unlock();
        if (busy) return NULL;
    }
    char name[MAX_PID_NAME_LEN];
    snprintf(name, sizeof(name), "%llu", (unsigned long long)process->pid);
    vfs_node_t root = procfs_private_process_node(NULL, name, file_dir, 0555, process, uid, gid, "proc_pid_dir");
    if (!root) return NULL;
    struct TaskFile { const char *name; const char *kind; uint16_t type; uint16_t mode; } files[] = {
        {"cmdline", "proc_cmdline", file_none, 0444},
        {"comm", "proc_comm", file_none, 0444},
        {"exe", "proc_exe", file_symlink, 0777},
        {"maps", "proc_maps", file_none, 0400},
        {"stat", "proc_stat", file_none, 0444},
        {"statm", "proc_statm", file_none, 0444},
        {"status", "proc_status", file_none, 0444},
        {"fd", "proc_fd_dir", file_dir, 0555},
    };
    for (const TaskFile &file : files)
        if (!procfs_private_process_node(root, file.name, file.type, file.mode, process, uid, gid, file.kind))
        {
            procfs_discard_private(root);
            return NULL;
        }
    list_t entry = list_alloc(root);
    if (!entry) { procfs_discard_private(root); return NULL; }
    vfs_node_t result = NULL;
    bool published = false;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        vfs_child_lock();
        if (!process->retired && !vfs_node_retiring_locked(procfs_root))
        {
            result = process->node;
            bool duplicate = false;
            for (list_t child = procfs_root->child; child; child = child->next)
            {
                auto *node = static_cast<vfs_node_t>(child->data);
                if (node && node->name && !strcmp(node->name, name)) { duplicate = true; break; }
            }
            if (!result && !duplicate)
            {
                root->parent = procfs_root;
                entry->next = procfs_root->child;
                if (entry->next) entry->next->prev = entry;
                procfs_root->child = entry;
                process->node = root;
                __atomic_store_n(&task->procfs_node, root, __ATOMIC_RELEASE);
                result = root;
                published = true;
            }
        }
        vfs_child_unlock();
    }
    if (!published) { free(entry); procfs_discard_private(root); }
    return result;
}

void procfs_update_task_list()
{
    if (!procfs_root || !pcb_group_queue || !get_current_task() || !scheduler_is_enabled()) return;
    size_t capacity = 0;
    {
        ProcfsSpinGuard guard(&pcb_group_queue->lock);
        for (lock_node *entry = pcb_group_queue->head; entry; entry = entry->next) ++capacity;
    }
    if (capacity > size_t(-1) / sizeof(uint64_t)) return;
    auto *pids = static_cast<uint64_t *>(malloc(capacity * sizeof(uint64_t)));
    if (capacity && !pids) return;
    size_t count = 0;
    {
        ProcfsSpinGuard guard(&pcb_group_queue->lock);
        for (lock_node *entry = pcb_group_queue->head; entry && count < capacity; entry = entry->next)
        {
            auto *task = static_cast<pcb_t>(entry->data);
            if (task && task->status != DEATH && task->status != OUT) pids[count++] = task->pid;
        }
    }
    for (size_t i = 0; i < count; ++i)
    {
        ProcfsOperation operation;
        if (operation.acquire(pids[i])) procfs_ensure_process(operation);
    }
    free(pids);
}

static void procfs_sort_numbers(size_t *numbers, size_t count)
{
    auto sift = [&](size_t root, size_t end)
    {
        while (root < end / 2)
        {
            size_t child = root * 2 + 1;
            if (child + 1 < end && numbers[child] < numbers[child + 1]) ++child;
            if (numbers[root] >= numbers[child]) break;
            size_t value = numbers[root]; numbers[root] = numbers[child]; numbers[child] = value;
            root = child;
        }
    };
    for (size_t i = count / 2; i; --i) sift(i - 1, count);
    for (size_t end = count; end > 1; --end)
    {
        size_t value = numbers[0]; numbers[0] = numbers[end - 1]; numbers[end - 1] = value;
        sift(0, end - 1);
    }
}

static bool procfs_fd_number(const char *name, size_t &number)
{
    if (!name || !*name) return false;
    number = 0;
    for (const char *p = name; *p; ++p)
    {
        if (*p < '0' || *p > '9' || number > (0x7fffffffU - size_t(*p - '0')) / 10) return false;
        number = number * 10 + size_t(*p - '0');
    }
    return true;
}

static bool procfs_has_number(const size_t *numbers, size_t count, size_t number)
{
    size_t first = 0;
    while (first < count)
    {
        size_t middle = first + (count - first) / 2;
        if (numbers[middle] == number) return true;
        if (numbers[middle] < number) first = middle + 1;
        else count = middle;
    }
    return false;
}

static void procfs_refresh_fd_dir(vfs_node_t directory, ProcfsOperation &operation)
{
    pcb_t task = operation.get();
    ProcfsProcessContext *process = operation.context();
    if (!directory || !task || !process) return;
    size_t capacity = 0, count = 0;
    if (fd_snapshot_numbers(task, NULL, 0, capacity)) return;
    if (capacity > size_t(-1) / sizeof(size_t)) return;
    auto *numbers = static_cast<size_t *>(malloc(capacity * sizeof(size_t)));
    if (capacity && !numbers) return;
    int error = fd_snapshot_numbers(task, numbers, capacity, count);
    if (error || count > capacity) { free(numbers); return; }
    procfs_sort_numbers(numbers, count);
    uint32_t uid = directory->owner, gid = directory->group;
    for (size_t i = 0; i < count; ++i)
    {
        char name[24];
        snprintf(name, sizeof(name), "%zu", numbers[i]);
        bool found = false;
        vfs_child_lock();
        for (list_t entry = directory->child; entry; entry = entry->next)
        {
            auto *node = static_cast<vfs_node_t>(entry->data);
            if (node && node->name && !strcmp(node->name, name))
            {
                if (!vfs_node_retiring_locked(node))
                {
                    node->owner = uid;
                    node->group = gid;
                }
                found = true;
                break;
            }
        }
        vfs_child_unlock();
        if (found) continue;
        vfs_node_t node = procfs_private_process_node(NULL, name, file_symlink, 0400, process, uid, gid, "proc_fd");
        if (!node) break;
        list_t link = list_alloc(node);
        if (!link) { procfs_discard_private(node); break; }
        bool published = false;
        {
            ProcfsSpinGuard guard(&process_context_lock);
            vfs_child_lock();
            bool duplicate = false;
            for (list_t entry = directory->child; entry; entry = entry->next)
            {
                auto *child = static_cast<vfs_node_t>(entry->data);
                if (child && child->name && !strcmp(child->name, name)) { duplicate = true; break; }
            }
            if (!process->retired && !duplicate && !vfs_node_retiring_locked(directory))
            {
                node->parent = directory;
                link->next = directory->child;
                if (link->next) link->next->prev = link;
                directory->child = link;
                published = true;
            }
            vfs_child_unlock();
        }
        if (!published) { free(link); procfs_discard_private(node); }
    }
    size_t child_count = 0;
    vfs_child_lock();
    for (list_t entry = directory->child; entry; entry = entry->next) ++child_count;
    vfs_child_unlock();
    if (child_count > size_t(-1) / sizeof(vfs_node_t)) { free(numbers); return; }
    auto *obsolete = static_cast<vfs_node_t *>(malloc(child_count * sizeof(vfs_node_t)));
    if (child_count && !obsolete) { free(numbers); return; }
    size_t obsolete_count = 0;
    vfs_child_lock();
    for (list_t entry = directory->child; entry && obsolete_count < child_count; entry = entry->next)
    {
        auto *node = static_cast<vfs_node_t>(entry->data);
        size_t number;
        if (node && procfs_fd_number(node->name, number) && !procfs_has_number(numbers, count, number) &&
            !vfs_node_retiring_locked(node) && node->refcount != uint32_t(-1))
        {
            ++node->refcount;
            obsolete[obsolete_count++] = node;
        }
    }
    vfs_child_unlock();
    for (size_t i = 0; i < obsolete_count; ++i)
        if (vfs_prune_child(directory, obsolete[i], 1)) vfs_close(obsolete[i]);
    free(obsolete);
    free(numbers);
}

static bool procfs_owner_quiescent(tcb_t task)
{
    return task && (task->status == DEATH || task->status == OUT) && !scheduler_task_referenced(task);
}

static bool procfs_reap_operation(tcb_t owner, bool all)
{
    ProcfsOperationRecord *record = NULL;
    ProcfsProcessContext *release = NULL;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        ProcfsOperationRecord **entry = &process_operations;
        while (*entry)
        {
            if ((all || (*entry)->owner == owner) && procfs_owner_quiescent((*entry)->owner))
            {
                record = *entry;
                *entry = record->next;
                ProcfsProcessContext *process = record->process;
                --process->active;
                if (!--process->references) release = process;
                break;
            }
            entry = &(*entry)->next;
        }
    }
    if (record) free(record);
    if (release) procfs_destroy_context(release);
    return record != NULL;
}

bool procfs_operation_thread_exit(tcb_t task)
{
    // The caller still owns the TCB; acquire/reap scans retain their owner
    // pointers under this same lock until every record has been unlinked.
    {
        ProcfsSpinGuard guard(&process_context_lock);
        if (!procfs_owner_quiescent(task)) return false;
    }
    while (procfs_reap_operation(task, false)) {}
    return true;
}

void procfs_reap_retired()
{
    if (!get_current_task() || !scheduler_is_enabled()) return;
    // FD removal finishes its owner/PCB-dependent registry work first. Once
    // extracted, finish_handle only closes its retained vnode and frees memory.
    fd_reap_abandoned_operations();
    while (procfs_reap_operation(NULL, true)) {}
    ProcfsProcessContext *process = NULL;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        for (ProcfsProcessContext *entry = retired_contexts; entry; entry = entry->next)
            if (!entry->collecting && !entry->active)
            {
                entry->collecting = true;
                process = entry;
                break;
            }
    }
    if (!process) return;
    int error = process->node ? vfs_try_free_detached(process->node) : 0;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        ProcfsProcessContext **link = &retired_contexts;
        while (*link && *link != process) link = &(*link)->next;
        if (*link)
        {
            *link = process->next;
            if (error)
            {
                // Rotate a busy tree so one held descriptor cannot starve
                // reclamation of every subsequently retired process cache.
                ProcfsProcessContext **tail = &retired_contexts;
                while (*tail) tail = &(*tail)->next;
                *tail = process;
                process->next = NULL;
                process->collecting = false;
            }
        }
    }
    if (!error) procfs_context_release(process); // Retired-queue hold.
}

void procfs_on_exit_task(pcb_t task)
{
    if (!task) return;
    ProcfsProcessContext *process = NULL;
    list_t detached = NULL;
    {
        ProcfsSpinGuard guard(&process_context_lock);
        ProcfsProcessContext **link = &process_contexts;
        while (*link && (*link)->identity != task) link = &(*link)->next;
        if (*link)
        {
            process = *link;
            *link = process->next;
            process->retired = true;
            process->task = NULL;
            // The transferred registry/queue hold remains exclusive to this
            // exit waiter until all live operations finish. No extra counter
            // increment (and therefore no overflow) is needed for the waiter.
            process->collecting = true;
            vfs_child_lock();
            if (process->node && procfs_root)
            {
                list_t entry = procfs_root->child;
                while (entry && entry->data != process->node) entry = entry->next;
                if (entry)
                {
                    if (entry->prev) entry->prev->next = entry->next;
                    else procfs_root->child = entry->next;
                    if (entry->next) entry->next->prev = entry->prev;
                    detached = entry;
                }
            }
            vfs_child_unlock();
            process->next = retired_contexts;
            retired_contexts = process; // Transfer published-registry hold.
        }
        __atomic_store_n(&task->procfs_node, static_cast<vfs_node_t>(NULL), __ATOMIC_RELEASE);
    }
    free(detached);
    if (!process) return;
    for (;;)
    {
        // A killed callback runs no C++ destructor. Reap its heap ticket only
        // after current/selected/previous-stack references prove quiescence.
        fd_reap_abandoned_operations();
        while (procfs_reap_operation(NULL, true)) {}
        bool active;
        {
            ProcfsSpinGuard guard(&process_context_lock);
            active = process->active != 0;
        }
        if (!active) break;
        scheduler_yield();
    }
    {
        ProcfsSpinGuard guard(&process_context_lock);
        process->collecting = false;
    }
    procfs_reap_retired();
}
