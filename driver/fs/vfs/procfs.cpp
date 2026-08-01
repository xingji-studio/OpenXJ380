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

static int procfs_id     = 0;
static int proc_self_id  = 0;
vfs_node_t procfs_root   = NULL;
spin_t     procfs_oplock = SPIN_INIT;
extern lock_queue   *pcb_group_queue;
extern XSK_SMP_INFO *xsi;

static size_t procfs_copy_content(void *addr, size_t offset, size_t size, const char *content, size_t content_len);
static void procfs_refresh_fd_dir(vfs_node_t fd_dir, pcb_t task);
static vfs_node_t procfs_current_self_target()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return NULL;
    return task->parent_group->procfs_node;
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

static bool procfs_node_is_task_dir(vfs_node_t node)
{
    if (node == NULL || node->parent != procfs_root || !(node->type & file_dir)) return false;
    if (node->name == NULL || node->name[0] == '\0') return false;
    for (const char *p = node->name; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
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
    uint64_t total = pcb_group_queue ? pcb_group_queue->size : 0;
    if (pcb_group_queue != NULL)
    {
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
        queue_foreach(task->thread_queue, node)
        {
            tcb_t thread = (tcb_t)node->data;
            if (thread != NULL && thread->user_info != NULL) return user_uid(thread->user_info);
        }
    }
    return user_uid(task_effective_user());
}

static uint32_t proc_task_gid(pcb_t task)
{
    if (task == NULL) return 0;
    if (task->thread_queue != NULL)
    {
        queue_foreach(task->thread_queue, node)
        {
            tcb_t thread = (tcb_t)node->data;
            if (thread != NULL && thread->user_info != NULL) return user_gid(thread->user_info);
        }
    }
    return user_gid(task_effective_user());
}

static void procfs_set_owner(vfs_node_t node, pcb_t task)
{
    if (node == NULL) return;
    node->owner = proc_task_uid(task);
    node->group = proc_task_gid(task);
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
    queue_foreach(task->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread != NULL) blocked |= thread->blocked;
    }
    return blocked;
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

char *proc_gen_maps_file(pcb_t task, size_t *content_len) {
    vma_t *vma = task->vma_manager.vma_list;

    size_t offset  = 0;
    size_t ctn_len = PAGE_SIZE;
    char  *buf     = (char*)malloc(ctn_len);

    while (vma) {
        vfs_node_t node = NULL;
        if (vma->vm_fd != -1) {
            fd_file_handle *fd_handle =
                (fd_file_handle*)queue_get(task->file_open, vma->vm_fd);
            if (fd_handle != NULL) node = fd_handle->node;
        }

        int len = sprintf(buf + offset, "%012lx-%012lx %s %08lx %02x:%02x %lu", vma->vm_start,
                          vma->vm_end, get_vma_permissions(vma), (unsigned long)vma->vm_offset, 0,
                          0, node ? node->inode : 0);

        if (offset + len > ctn_len) {
            ctn_len = (offset + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            buf     = (char*)realloc(buf, ctn_len);
        }
        offset += len;

        const char *pathname = vma->vm_name;
        if (pathname && strlen(pathname) > 0) {
            len = sprintf(buf + offset, "%*s%s", 15, "", pathname);
            if (offset + len > ctn_len) {
                ctn_len = (offset + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                buf     = (char*)realloc(buf, ctn_len);
            }
            offset += len;
        }

        len = sprintf(buf + offset, "\n");
        if (offset + len > ctn_len) {
            ctn_len = (offset + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            buf     = (char*)realloc(buf, ctn_len);
        }
        offset += len;

        vma = vma->vm_next;
    }

    *content_len = offset;

    return buf;
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
                      (unsigned long long)(task->parent_task ? task->parent_task->pid : 0),
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
                           task->parent_task ? (int)task->parent_task->pid : 0, // ppid
                           (int)task->pid,                              // pgrp
                           task->parent_task ? (int)task->parent_task->pid : (int)task->pid, // session
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
        "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
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

errno_t procfs_mount(const char *src, vfs_node_t node) {
    if (src != (void *)PROC_REGISTER_ID) return VFS_STATUS_FAILED;
    procfs_root = node;

    procfs_root->fsid = procfs_id;
    procfs_root->owner = 0;
    procfs_root->group = 0;

    vfs_node_t procfs_self = vfs_node_alloc(procfs_root, "self");
    procfs_self->type      = file_symlink;
    procfs_self->mode      = 0644;
    procfs_self->linkto    = NULL;
    procfs_self->fsid      = proc_self_id;
    procfs_self->owner     = 0;
    procfs_self->group     = 0;

    struct RootProcFile { const char *name; const char *handle; } root_files[] = {
        {"cmdline", "cmdline"},
        {"cpuinfo", "cpuinfo"},
        {"mounts", "mounts"},
        {"filesystems", "filesystems"},
        {"meminfo", "meminfo"},
        {"uptime", "uptime"},
        {"loadavg", "loadavg"},
        {"stat", "stat"},
        {"version", "version"},
    };

    for (size_t i = 0; i < sizeof(root_files) / sizeof(root_files[0]); i++)
    {
        vfs_node_t file = vfs_node_alloc(procfs_root, root_files[i].name);
        file->type = file_none;
        file->mode = 0444;
        file->owner = 0;
        file->group = 0;
        file->fsid = procfs_id;
        proc_handle_t *file_handle = (proc_handle_t *)malloc(sizeof(proc_handle_t));
        file->handle = file_handle;
        file_handle->task = NULL;
        sprintf(file_handle->name, "%s", root_files[i].handle);
    }

    vfs_node_t sys_dir = vfs_node_alloc(procfs_root, "sys");
    sys_dir->type = file_dir;
    sys_dir->mode = 0555;
    sys_dir->owner = 0;
    sys_dir->group = 0;
    sys_dir->fsid = procfs_id;

    vfs_node_t kernel_dir = vfs_node_alloc(sys_dir, "kernel");
    kernel_dir->type = file_dir;
    kernel_dir->mode = 0555;
    kernel_dir->owner = 0;
    kernel_dir->group = 0;
    kernel_dir->fsid = procfs_id;

    struct SysKernelProcFile { const char *name; const char *handle; } sys_kernel_files[] = {
        {"ostype", "sys_kernel_ostype"},
        {"osrelease", "sys_kernel_osrelease"},
        {"version", "sys_kernel_version"},
        {"hostname", "sys_kernel_hostname"},
    };

    for (size_t i = 0; i < sizeof(sys_kernel_files) / sizeof(sys_kernel_files[0]); i++)
    {
        vfs_node_t file = vfs_node_alloc(kernel_dir, sys_kernel_files[i].name);
        file->type = file_none;
        file->mode = 0444;
        file->owner = 0;
        file->group = 0;
        file->fsid = procfs_id;
        proc_handle_t *file_handle = (proc_handle_t *)malloc(sizeof(proc_handle_t));
        file->handle = file_handle;
        file_handle->task = NULL;
        sprintf(file_handle->name, "%s", sys_kernel_files[i].handle);
    }

    procfs_update_task_list();

    return VFS_STATUS_SUCCESS;
}

void procfs_open(void *parent, const char *name, vfs_node_t node) {
    UNUSED(parent, name);
    if (node == procfs_root) procfs_update_task_list();
    if (procfs_node_is_task_dir(node))
    {
        int pid = atoi(node->name);
        pcb_t task = found_pcb(pid);
        if (task != NULL) procfs_on_new_task(task);
    }
    proc_handle_t *handle = (proc_handle_t *)node->handle;
    if (handle != NULL && !strcmp(handle->name, "proc_fd_dir") && handle->task != NULL)
        procfs_refresh_fd_dir(node, handle->task);
}

void procfs_close(void *current) {
    UNUSED(current);
}

size_t procfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size) {
    if (node == NULL || addr == NULL) return VFS_STATUS_FAILED;

    if (node->handle != NULL)
    {
        proc_handle_t *handle = (proc_handle_t *)node->handle;
        if (!strcmp(handle->name, "proc_exe"))
        {
            const char *target = handle->task && handle->task->exe_path ? handle->task->exe_path : "";
            return procfs_copy_content(addr, offset, size, target, strlen(target));
        }
        if (!strcmp(handle->name, "proc_fd"))
        {
            if (handle->task == NULL || handle->task->file_open == NULL || node->name == NULL)
                return VFS_STATUS_FAILED;
            int fdnum = atoi(node->name);
            fd_file_handle *fh = (fd_file_handle *)queue_get(handle->task->file_open, fdnum);
            if (fh == NULL || fh->node == NULL) return VFS_STATUS_FAILED;
            char target[256];
            if (fh->node->type & file_pipe)
                sprintf(target, "pipe:[%llu]", (unsigned long long)fh->node->inode);
            else if (fh->node->type & file_socket)
                sprintf(target, "socket:[%llu]", (unsigned long long)fh->node->inode);
            else if (fh->node->name && fh->node->name[0])
                sprintf(target, "/%s", fh->node->name);
            else
                sprintf(target, "anon_inode:[%llu]", (unsigned long long)fh->node->inode);
            return procfs_copy_content(addr, offset, size, target, strlen(target));
        }
    }

    if (node->linkto != NULL)
    {
        char target[MAX_PID_NAME_LEN + 8];
        sprintf(target, "/proc/%s", node->linkto->name);
        return procfs_copy_content(addr, offset, size, target, strlen(target));
    }
    return VFS_STATUS_FAILED;
}

size_t procfs_write(void *file, const void *addr, size_t offset, size_t size) {
    UNUSED(file, addr, offset, size);
    return size;
}

size_t procfs_read(void *file, void *addr, size_t offset, size_t size) {
    proc_handle_t *handle = (proc_handle_t *)file;
    if (!handle) { return VFS_STATUS_FAILED; }
    pcb_t task;
    if (handle->task == NULL) {
        task = get_current_task()->parent_group;
    } else {
        task = handle->task;
    }

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
        char  *content     = proc_gen_maps_file(handle->task, &content_len);
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
        node->size = procfs_task_cmdline_len(handle->task);
    else if (!strcmp(handle->name, "proc_comm"))
        node->size = strlen(handle->task->name) + 1;
    else if (!strcmp(handle->name, "proc_exe"))
        node->size = handle->task && handle->task->exe_path ? strlen(handle->task->exe_path) : 0;
    else if (!strcmp(handle->name, "proc_statm")) {
        size_t content_len = 0;
        char *content = proc_gen_statm_file(handle->task, &content_len);
        free(content);
        node->size = content_len;
    }
    else if (!strcmp(handle->name, "proc_maps")) {
        size_t content_len = 0;
        char  *content     = proc_gen_maps_file(handle->task, &content_len);
        free(content);
        node->size = content_len;
    } else if (!strcmp(handle->name, "dri_name"))
        node->size = strlen("cpkernel_drm");
    else if (!strcmp(handle->name, "proc_fd_dir"))
        node->size = 0;
    else if (!strcmp(handle->name, "proc_fd"))
        node->size = 64;
    else if (!strcmp(handle->name, "proc_stat")) {
        size_t content_len = 0;
        char  *content     = proc_gen_stat_file(handle->task, &content_len);
        node->size         = content_len;
        free(content);
    } else if (!strcmp(handle->name, "proc_status")) {
        size_t content_len = 0;
        char  *content     = proc_gen_status_file(handle->task, &content_len);
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

void procfs_setup() {
    procfs_id    = vfs_regist("proc", &procfs_callbacks, PROC_REGISTER_ID, 0x9fa0);
    proc_self_id = vfs_regist("proc_self", &procfs_self_callbacks, PROC_REGISTER_ID, 0x0);
    vfs_node_t proot;
    vfs_mkdir("/proc");
    proot=vfs_open("/proc");
    procfs_mount((const char*)PROC_REGISTER_ID,proot);
    if (procfs_id == VFS_STATUS_FAILED || proc_self_id == VFS_STATUS_FAILED) {
        write_serial_fmt("procfs register error\n");
    }
}

void procfs_update_task_list() {
    if (procfs_root == NULL || pcb_group_queue == NULL) return;
    queue_foreach(pcb_group_queue, node)
    {
        procfs_on_new_task((pcb_t)node->data);
    }
}

static void procfs_refresh_fd_dir(vfs_node_t fd_dir, pcb_t task)
{
    if (fd_dir == NULL || task == NULL) return;
    list_t child = fd_dir->child;
    while (child != NULL) {
        list_t next = child->next;
        vfs_node_t child_node = (vfs_node_t)child->data;
        if (child_node != NULL) {
            if (child_node->handle) {
                free(child_node->handle);
                child_node->handle = NULL;
            }
            free(child_node);
        }
        fd_dir->child = list_delete_node(fd_dir->child, child);
        child = next;
    }

    if (task->file_open == NULL) return;
    spin_lock(&task->file_open->lock);
    lock_node *ln = task->file_open->head;
    while (ln != NULL) {
        fd_file_handle *fh = (fd_file_handle *)ln->data;
        if (fh != NULL && fh->node != NULL) {
            char fdname[16];
            sprintf(fdname, "%zu", fh->fd);
            vfs_node_t fd_entry = vfs_child_append(fd_dir, fdname, NULL);
            fd_entry->type = file_symlink;
            fd_entry->mode = 0400;
            fd_entry->fsid = procfs_id;
            fd_entry->owner = task->procfs_node ? task->procfs_node->owner : 0;
            fd_entry->group = task->procfs_node ? task->procfs_node->group : 0;
            proc_handle_t *handle = (proc_handle_t *)malloc(sizeof(proc_handle_t));
            handle->task = task;
            sprintf(handle->name, "proc_fd");
            fd_entry->handle = handle;
        }
        ln = ln->next;
    }
    spin_unlock(&task->file_open->lock);
}

void procfs_on_new_task(pcb_t task) {
    if (procfs_root == NULL || task == NULL) return;

    char name[MAX_PID_NAME_LEN];
    sprintf(name, "%llu", (unsigned long long)task->pid);

    vfs_node_t node = vfs_do_search(procfs_root, name);
    if (node == NULL) {
        node       = vfs_child_append(procfs_root, name, NULL);
        node->type = file_dir;
        node->mode = 0555;
        node->fsid = procfs_id;
    }
    procfs_set_owner(node, task);
    task->procfs_node = node;

    struct TaskProcFile { const char *name; const char *handle; uint16_t type; uint16_t mode; } task_files[] = {
        {"cmdline", "proc_cmdline", file_none, 0444},
        {"comm", "proc_comm", file_none, 0444},
        {"exe", "proc_exe", file_symlink, 0777},
        {"maps", "proc_maps", file_none, 0400},
        {"stat", "proc_stat", file_none, 0444},
        {"statm", "proc_statm", file_none, 0444},
        {"status", "proc_status", file_none, 0444},
    };

    for (size_t i = 0; i < sizeof(task_files) / sizeof(task_files[0]); i++)
    {
        vfs_node_t file = vfs_do_search(node, task_files[i].name);
        if (file == NULL) file = vfs_child_append(node, task_files[i].name, NULL);
        file->type = task_files[i].type;
        file->mode = task_files[i].mode;
        file->fsid = procfs_id;
        procfs_set_owner(file, task);
        proc_handle_t *handle = (proc_handle_t *)file->handle;
        if (handle == NULL)
        {
            handle = (proc_handle_t *)malloc(sizeof(proc_handle_t));
            file->handle = handle;
        }
        handle->task = task;
        sprintf(handle->name, "%s", task_files[i].handle);
    }

    vfs_node_t fd_dir = vfs_do_search(node, "fd");
    if (fd_dir == NULL) {
        fd_dir       = vfs_child_append(node, "fd", NULL);
        fd_dir->type = file_dir;
        fd_dir->mode = 0555;
        fd_dir->fsid = procfs_id;
        procfs_set_owner(fd_dir, task);
        proc_handle_t *fd_handle = (proc_handle_t *)malloc(sizeof(proc_handle_t));
        fd_handle->task = task;
        sprintf(fd_handle->name, "proc_fd_dir");
        fd_dir->handle = fd_handle;
    }
    procfs_refresh_fd_dir(fd_dir, task);
}

void procfs_on_exit_task(pcb_t task) {
    if (procfs_root == NULL) return;
    spin_lock(&procfs_oplock);

    char name[MAX_PID_NAME_LEN];
    sprintf(name, "%llu", (unsigned long long)task->pid);

    vfs_node_t node = vfs_do_search(procfs_root, name);
    if (node && node->parent) {
        node->parent->child = list_delete(node->parent->child, node);
        vfs_free(node);
        task->procfs_node = NULL;
    }

    spin_unlock(&procfs_oplock);
}
