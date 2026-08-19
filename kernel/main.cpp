#include "build_settings.h"
#include "build_config.h"
#include <ahci/ahci.h>
#include <cpu/regio.h>
#include <device.h>
#include <cpu/fsgsbase.h>
#include <console.h>
#include <dlinker.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <hda/hda.h>
#include <krlibc.h>
#include <mm/alloc/alloc.h>
#include <mm/frame.h>
#include <nvme/nvme.h>
#include <openxj380/config.h>
#include <openxj380/socket.h>
#include <openxj380/syscall.h>
#include <pci/pci.h>
#include <pipe.h>
#include <power.h>
#include <proto.hpp>
#include <pty.h>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <syscall/signal.h>
#include <syscall/pxapi.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/runfile.h>
#include <user/settings.h>
#include <installer_mode.h>

const FrameBufferConfig *fbc_addr;

extern int           scheduler_is_ready;
extern XSK_SMP_INFO *xsi;
#define NULL 0
uint64_t *saved_mtrrs;
void     *temp_stack[MAX_CPU_NUM];
bool      no_interrupt = false;

static OpenXJ380MouseInterruptHook g_openxj380_mouse_hook = NULL;
static OpenXJ380KeyboardInterruptHook g_openxj380_keyboard_hook = NULL;
static OpenXJ380SyscallHook g_openxj380_syscall_hook = NULL;
extern EFI_SYSTEM_TABLE *EFI_ST;
extern BOOT_CONFIG *EFI_BC;

extern "C" int OpenXJ380Socket_RegisterMouseHook(OpenXJ380MouseInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    if (hook == NULL) return -1;
    OpenXJ380MouseInterruptHook expected = NULL;
    return __atomic_compare_exchange_n(&g_openxj380_mouse_hook, &expected, hook, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)
               ? 0
               : -1;
#else
    (void)hook;
    return -1;
#endif
}

extern "C" int OpenXJ380Socket_RegisterKeyboardHook(OpenXJ380KeyboardInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    if (hook == NULL) return -1;
    OpenXJ380KeyboardInterruptHook expected = NULL;
    return __atomic_compare_exchange_n(&g_openxj380_keyboard_hook, &expected, hook, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)
               ? 0
               : -1;
#else
    (void)hook;
    return -1;
#endif
}

extern "C" void OpenXJ380Socket_UnregisterMouseHook(OpenXJ380MouseInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380MouseInterruptHook expected = hook;
    __atomic_compare_exchange_n(&g_openxj380_mouse_hook, &expected, NULL, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
#else
    (void)hook;
#endif
}

extern "C" void OpenXJ380Socket_UnregisterKeyboardHook(OpenXJ380KeyboardInterruptHook hook)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380KeyboardInterruptHook expected = hook;
    __atomic_compare_exchange_n(&g_openxj380_keyboard_hook, &expected, NULL, false, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
#else
    (void)hook;
#endif
}

extern "C" bool OpenXJ380Socket_MouseInterrupte(const OpenXJ380MouseInterruptInfo *event)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380MouseInterruptHook hook = __atomic_load_n(&g_openxj380_mouse_hook, __ATOMIC_ACQUIRE);
    return hook != NULL && event != NULL && hook(event);
#else
    (void)event;
    return false;
#endif
}

extern "C" bool OpenXJ380Socket_KeyboardInterrupt(const OpenXJ380KeyboardInterruptInfo *event)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380KeyboardInterruptHook hook = __atomic_load_n(&g_openxj380_keyboard_hook, __ATOMIC_ACQUIRE);
    return hook != NULL && event != NULL && hook(event);
#else
    (void)event;
    return false;
#endif
}

extern "C" const void *OpenXJ380Socket_FramebufferConfig()
{
    return fbc_addr;
}

extern "C" uint64_t OpenXJ380Socket_PowerAction(uint64_t action)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)action;
    return (uint64_t)-1;
#else
    if (EFI_ST == NULL || EFI_BC == NULL) return (uint64_t)-1;
    if (action == XPOWER_REBOOT) power_reboot(EFI_ST, EFI_BC);
    if (action == XPOWER_SHUTDOWN) power_shutdown(EFI_ST, EFI_BC);
    return (uint64_t)-1;
#endif
}

extern "C" int OpenXJ380Socket_RegisterSyscallHook(OpenXJ380SyscallHook hook)
{
#if !OPENXJ380_GUI_DISABLED
    if (hook == NULL) return -1;
    OpenXJ380SyscallHook expected = NULL;
    return __atomic_compare_exchange_n(&g_openxj380_syscall_hook, &expected, hook, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)
               ? 0
               : -1;
#else
    (void)hook;
    return -1;
#endif
}

extern "C" void OpenXJ380Socket_UnregisterSyscallHook(OpenXJ380SyscallHook hook)
{
#if !OPENXJ380_GUI_DISABLED
    OpenXJ380SyscallHook expected = hook;
    __atomic_compare_exchange_n(&g_openxj380_syscall_hook, &expected, NULL, false, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
#else
    (void)hook;
#endif
}

extern "C" bool OpenXJ380Socket_DispatchSyscall(uint64_t syscall_number, struct X64_REGS *regs)
{
#if !OPENXJ380_GUI_DISABLED
    OpenXJ380SyscallHook hook = __atomic_load_n(&g_openxj380_syscall_hook, __ATOMIC_ACQUIRE);
    return hook != NULL && regs != NULL && hook(syscall_number, regs);
#else
    (void)syscall_number;
    (void)regs;
    return false;
#endif
}

EXPORT_SYMBOL(OpenXJ380Socket_RegisterMouseHook);
EXPORT_SYMBOL(OpenXJ380Socket_RegisterKeyboardHook);
EXPORT_SYMBOL(OpenXJ380Socket_MouseInterrupte);
EXPORT_SYMBOL(OpenXJ380Socket_KeyboardInterrupt);
EXPORT_SYMBOL(OpenXJ380Socket_UnregisterMouseHook);
EXPORT_SYMBOL(OpenXJ380Socket_UnregisterKeyboardHook);
EXPORT_SYMBOL(OpenXJ380Socket_FramebufferConfig);
EXPORT_SYMBOL(OpenXJ380Socket_PowerAction);
EXPORT_SYMBOL(OpenXJ380Socket_RegisterSyscallHook);
EXPORT_SYMBOL(OpenXJ380Socket_UnregisterSyscallHook);

extern bool allow_to_flush;
extern void ahci_set_accel(bool enabled);
extern bool ahci_is_qemu_environment();
EFI_SYSTEM_TABLE *EFI_ST;
extern BOOT_CONFIG *EFI_BC;

extern UserInfo *current_user;

static constexpr size_t INIT_CONFIG_MAX_BYTES = 4096;
static constexpr size_t INIT_CONFIG_MAX_MODULES = 16;

struct initial_program_config
{
    char path[256];
    char required_modules[INIT_CONFIG_MAX_MODULES][32];
    size_t required_module_count;
};

static bool init_config_module_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    for (const char *cursor = name; *cursor != '\0'; ++cursor)
    {
        if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_' || *cursor == '-'))
            return false;
    }
    return true;
}

static char *init_config_trim(char *value)
{
    while (*value == ' ' || *value == '\t' || *value == '\r') ++value;
    char *end = value + strlen(value);
    while (end != value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end = '\0';
    return value;
}

static bool load_initial_program_config(initial_program_config *config, const char **error)
{
    if (config == NULL || error == NULL) return false;
    memset(config, 0, sizeof(*config));
    *error = "invalid configuration state";

    vfs_node_t file = vfs_open("/system/config/init.conf");
    if (file == NULL) { *error = "missing /system/config/init.conf"; return false; }
    if (file->size == 0 || file->size >= INIT_CONFIG_MAX_BYTES)
    {
        vfs_close(file);
        *error = "invalid init.conf size";
        return false;
    }

    char content[INIT_CONFIG_MAX_BYTES];
    size_t read = vfs_read(file, content, 0, file->size);
    vfs_close(file);
    if (read != file->size) { *error = "failed to read init.conf"; return false; }
    content[read] = '\0';

    bool have_init = false;
    for (char *line = content; line != NULL;)
    {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        char *comment = strchr(line, '#');
        if (comment != NULL) *comment = '\0';
        char *entry = init_config_trim(line);
        if (entry[0] != '\0')
        {
            char *equals = strchr(entry, '=');
            if (equals == NULL || strchr(equals + 1, '=') != NULL)
            {
                *error = "malformed init.conf entry";
                return false;
            }
            *equals = '\0';
            char *key = init_config_trim(entry);
            char *value = init_config_trim(equals + 1);
            if (value[0] == '\0') { *error = "empty init.conf value"; return false; }

            if (strcmp(key, "init") == 0)
            {
                if (have_init || value[0] != '/') { *error = "invalid init program path"; return false; }
                for (const char *cursor = value; *cursor != '\0'; ++cursor)
                    if (*cursor == ' ' || *cursor == '\t') { *error = "invalid init program path"; return false; }
                if (strlen(value) >= sizeof(config->path)) { *error = "init program path too long"; return false; }
                strcpy(config->path, value);
                have_init = true;
            }
            else if (strcmp(key, "require_module") == 0)
            {
                if (!init_config_module_name_valid(value) || config->required_module_count == INIT_CONFIG_MAX_MODULES)
                {
                    *error = "invalid required module";
                    return false;
                }
                for (size_t i = 0; i < config->required_module_count; ++i)
                    if (strcmp(config->required_modules[i], value) == 0)
                    {
                        *error = "duplicate required module";
                        return false;
                    }
                strcpy(config->required_modules[config->required_module_count++], value);
            }
            else
            {
                *error = "unknown init.conf key";
                return false;
            }
        }
        line = next;
    }

    if (!have_init) { *error = "missing init program"; return false; }
    return true;
}

[[noreturn]] static void shutdown_boot_failure(const char *reason)
{
    write_serial_fmt("BOOT: initial program startup failed: %s\n", reason);
    power_shutdown(EFI_ST, EFI_BC);
}

void init_cpu()
{
    __asm__ __volatile__("movq %%cr0, %%rax\n\t"
                         "and $0xFFF3, %%ax	\n\t" // clear coprocessor emulation CR0.EM and CR0.TS
                         "or $0x2, %%ax\n\t"      // set coprocessor monitoring  CR0.MP
                         "movq %%rax, %%cr0\n\t"
                         "movq %%cr4, %%rax\n\t"
                         "or $(3 << 9), %%ax\n\t" // set CR4.OSFXSR and CR4.OSXMMEXCPT at the same time
                         "movq %%rax, %%cr4\n\t" ::
                             : "rax");

    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (ecx & (1U << 26))
    {
        uint64_t cr4 = 0;
        __asm__ __volatile__("movq %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 18); // CR4.OSXSAVE: allow xgetbv/xsetbv/xsave in user-visible CPU feature paths.
        __asm__ __volatile__("movq %0, %%cr4" ::"r"(cr4) : "memory");

        uint32_t xcr0_lo = 0x3; // x87 + SSE only; context switching still uses fxsave/fxrstor.
        uint32_t xcr0_hi = 0;
        __asm__ __volatile__("xsetbv" ::"c"(0), "a"(xcr0_lo), "d"(xcr0_hi) : "memory");
    }

    if (has_fsgsbase())
    {
        uint64_t cr4 = 0;
        __asm__ __volatile__("movq %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 16);
        __asm__ __volatile__("movq %0, %%cr4" ::"r"(cr4) : "memory");
        read_fsbase  = rdfsbase;
        write_fsbase = wrfsbase;
        read_gsbase  = rdgsbase;
        write_gsbase = wrgsbase;
    }
}

static constexpr uint64_t KERNEL_HEAP_EXTEND_CHUNK = 64UL * 1024UL * 1024UL;
static constexpr uint64_t KERNEL_HEAP_MAX_BYTES    = 2048UL * 1024UL * 1024UL;

static uint64_t kernel_heap_mapped_bytes = 0;
static spin_t   kernel_heap_extend_lock  = SPIN_INIT;
static bool     kernel_heap_initialized  = false;

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void init_heap()
{
    page_map_range_to_random(get_kernel_pagedir(), KERNEL_HEAP_BASE, KERNEL_HEAP_BYTES, KERNEL_PTE_FLAGS);
    memset((void *)KERNEL_HEAP_BASE, 0, KERNEL_HEAP_BYTES);
    heap_init((uint8_t *)KERNEL_HEAP_BASE, KERNEL_HEAP_BYTES);
    kernel_heap_mapped_bytes = KERNEL_HEAP_BYTES;
    kernel_heap_initialized  = true;
}

bool kernel_heap_ready()
{
    return kernel_heap_initialized;
}

bool kernel_heap_extend(size_t min_bytes)
{
    if (!kernel_heap_initialized) return false;

    uint64_t extend_bytes = align_up_u64((uint64_t)min_bytes, PAGE_SIZE);
    if (extend_bytes < KERNEL_HEAP_EXTEND_CHUNK) extend_bytes = KERNEL_HEAP_EXTEND_CHUNK;

    spin_lock(&kernel_heap_extend_lock);

    if (kernel_heap_mapped_bytes + extend_bytes > KERNEL_HEAP_MAX_BYTES)
    {
        spin_unlock(&kernel_heap_extend_lock);
        write_serial_string("Kernel heap extend refused: limit reached.\n");
        return false;
    }

    uint64_t extend_base = KERNEL_HEAP_BASE + kernel_heap_mapped_bytes;
    uint64_t mapped      = 0;
    for (; mapped < extend_bytes; mapped += PAGE_SIZE)
    {
        uint64_t frame = alloc_frames(1);
        if (frame == 0)
        {
            unmap_page_range(get_kernel_pagedir(), extend_base, mapped);
            spin_unlock(&kernel_heap_extend_lock);
            write_serial_string("Kernel heap extend failed: cannot allocate frames.\n");
            return false;
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        page_map_to(get_kernel_pagedir(), extend_base + mapped, frame, KERNEL_PTE_FLAGS | PTE_FRAME_ALLOCATED);
    }
    memset((void *)extend_base, 0, extend_bytes);

    bool ok = heap_extend((uint8_t *)extend_base, extend_bytes);
    if (ok)
    {
        kernel_heap_mapped_bytes += extend_bytes;
        write_serial_string("Kernel heap extended by ");
        write_serial_dec(extend_bytes / 1024 / 1024);
        write_serial_string(" MiB, total ");
        write_serial_dec(kernel_heap_mapped_bytes / 1024 / 1024);
        write_serial_string(" MiB.\n");
    }
    else
    {
        unmap_page_range(get_kernel_pagedir(), extend_base, extend_bytes);
        write_serial_string("Kernel heap extend failed.\n");
    }

    spin_unlock(&kernel_heap_extend_lock);
    return ok;
}

extern void     nvme_setup();
#if CONFIG_KERNEL_BUILTIN_XHCI && !OPENXJ380_INPUT_OUTPUT_DISABLED
extern int      xhci_setup();
extern void     xhci_start_workers();
#endif
extern void     mount_root();
extern void     stdio_init();
extern int      socketfs_setup();
extern int      procfs_setup();
extern int      dnsfs_setup();
extern int      nmfs_setup();
extern int      tmpfs_setup();
BOOT_CONFIG *EFI_BC;
static BOOT_CONFIG g_kernel_boot_config;

extern "C" void KernelMain(const FrameBufferConfig &fbc, EFI_SYSTEM_TABLE &SystemTable, BOOT_CONFIG &BootConfigIn)
{
    memcpy(&g_kernel_boot_config, &BootConfigIn, sizeof(g_kernel_boot_config));
    BOOT_CONFIG &BootConfig = g_kernel_boot_config;

    fbc_addr = &fbc;
    console_init(fbc);
    no_interrupt = true;
    disable_intr();
    disable_scheduler();

    EFI_ST = &SystemTable;
    EFI_BC = &BootConfig;

    init_cpu();
    init_idt();
    init_gdt();
    init_hpet(BootConfig.HPET);
    init_apic(BootConfig.MADT);

    init_hhdm();
    init_frame(BootConfig.MemoryMap);
    init_heap();

    write_serial_string("BOOT: device_manager_init begin\n");
    device_manager_init();
    write_serial_string("BOOT: device_manager_init done\n");
    write_serial_string("BOOT: vfs_init begin\n");
    vfs_init();
    write_serial_string("BOOT: vfs_init done\n");
    write_serial_string("BOOT: fatfs_init begin\n");
    fatfs_init();
    write_serial_string("BOOT: fatfs_init done\n");
    write_serial_string("BOOT: devfs_setup begin\n");
    devfs_setup();
    write_serial_string("BOOT: devfs_setup done\n");

    write_serial_string("BOOT: pci_setup begin\n");
    pci_setup(BootConfig.MCFG);
    write_serial_string("BOOT: pci_setup done\n");
    write_serial_string("BOOT: pci_init begin\n");
    pci_init();
    write_serial_string("BOOT: pci_init done\n");
    write_serial_fmt("VM hint: %s\n", BootConfig.is_qemu == 1 ? "QEMU/BOCHS" : "generic/VMware/real hardware");
    if (BootConfig.boot_flags != 0) write_serial_fmt("Boot option flags: 0x%llx\n", BootConfig.boot_flags);
    if ((BootConfig.boot_flags & BOOT_FLAG_LAST_KNOWN_GOOD) != 0)
        write_serial_string("BOOT: last known good configuration selected\n");
    ahci_set_environment(BootConfig.is_qemu == 1);
    write_serial_string("BOOT: ahci_setup begin\n");
    ahci_setup();
    write_serial_string("BOOT: ahci_setup done\n");
    ahci_set_accel(false);
    write_serial_string("BOOT: nvme_setup begin\n");
    nvme_setup();
    write_serial_string("BOOT: nvme_setup done\n");

    extern void ide_setup(void);
    write_serial_string("BOOT: ide_setup begin\n");
    ide_setup();
    write_serial_string("BOOT: ide_setup done\n");
#if CONFIG_KERNEL_BUILTIN_XHCI && !OPENXJ380_INPUT_OUTPUT_DISABLED
    write_serial_string("BOOT: xhci_setup begin\n");
    xhci_setup();
    write_serial_string("BOOT: xhci_setup done\n");
#endif
    write_serial_string("BOOT: partition_init begin\n");
    partition_init();
    write_serial_string("BOOT: partition_init done\n");
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    // HDA 驱动现在会在初始化阶段自行完成注册，这里只需要启动探测即可。
    hda_init();
    hda_regist();
#endif

    keyboard_init();
    mouse_init();

    disable_intr();

    process_setup();

    // 超绝偷懒解法，等有内存管理且在这之前初始化的时候叫我来改
    saved_mtrrs = (uint64_t *)((uint64_t)BootConfig.saved_mtrrs + 0xffff800000000000);
    for (int i = 0; i < MAX_CPU_NUM; ++i)
        temp_stack[i] = BootConfig.temp_stack[i];

    init_smp(BootConfig.MADT);

    const size_t idle_alloc_size = (sizeof(struct thread_control_block) + 15ULL) & ~15ULL;
    tcb_t idle_thread            = (tcb_t)aligned_alloc(16, idle_alloc_size);
    if (idle_thread != NULL) memset(idle_thread, 0, sizeof(struct thread_control_block));
    if (idle_thread == NULL) while (true) { __asm__ volatile("hlt"); }
    idle_thread->tid          = __atomic_fetch_add(&now_tid, 1, __ATOMIC_SEQ_CST);
    idle_thread->parent_group = kernel_group;
    idle_thread->kernel_stack = get_rsp();
    idle_thread->status       = RUNNING;
    strcpy(idle_thread->name, "idle thread");
    idle_thread->fs           = 0x10;
    idle_thread->fs_base      = 0;
    idle_thread->group_index  = queue_enqueue(kernel_group->thread_queue, idle_thread);
    idle_thread->context0.rsp = get_rsp();
    set_kernel_stack(get_rsp());
    idle_thread->queue_index = queue_enqueue_ref(get_current_cpu()->scheduler_queue, idle_thread, &idle_thread->sched_node);

    idle_thread->str_cwd = "/";
    idle_thread->cwd     = rootdir;
    save_fpu_context(&idle_thread->fpu_context);

    get_current_cpu()->current_task = idle_thread;
    write_kgsbase((uint64_t)get_current_cpu());

    memset(phys_to_virt(get_cr3()), 0, PAGE_SIZE / 2);

    disable_scheduler();

#if CONFIG_KERNEL_BUILTIN_XHCI && !OPENXJ380_INPUT_OUTPUT_DISABLED
    xhci_start_workers();
#endif

    mount_root();
    initial_program_config initial_program = {};
    const char *initial_program_error = NULL;
    if (!load_initial_program_config(&initial_program, &initial_program_error)) shutdown_boot_failure(initial_program_error);
    if (!installer_root_is_tmpfs_ready()) tmpfs_setup();
    pipefs_setup();
    pty_init();
    fsgsbase_init();
    socketfs_setup();
    procfs_setup();
    dnsfs_setup();
    nmfs_setup();
    stdio_init();

    signal_init();
    init_syscall();
    init_message();

    if (!OPENXJ380_INPUT_OUTPUT_DISABLED &&
        (BootConfig.boot_flags & (BOOT_FLAG_SAFE_MODE | BOOT_FLAG_DISABLE_KMOD | BOOT_FLAG_INSTALLER)) == 0) {
        module_setup();
        dlinker_init();
        load_all_kernel_module();
    } else {
        write_serial_string("BOOT: kernel modules skipped by boot menu\n");
    }

    for (size_t i = 0; i < initial_program.required_module_count; ++i)
    {
        if (!kernel_module_loaded(initial_program.required_modules[i]))
        {
            write_serial_fmt("BOOT: required module unavailable: %s\n", initial_program.required_modules[i]);
            shutdown_boot_failure("required module unavailable");
        }
    }

    while (true)
    {
        __asm__ volatile("pause");
        if (scheduler_is_ready == xsi->cpu_count) break;
    }

    init_reaper();
    write_serial_string("Process Reaper is ready\n");

    if (BootConfig.is_qemu == 1) pr_warn("XJ380 is running under the QEMU/BOCHS.\n");

    if ((BootConfig.boot_flags & (BOOT_FLAG_SAFE_MODE | BOOT_FLAG_SAFE_STORAGE_IO)) != 0) {
        write_serial_string("AHCI: keeping conservative IO path by boot menu\n");
    } else if (ahci_is_qemu_environment()) {
        ahci_set_accel(true);
    } else {
        write_serial_string("AHCI: keeping conservative IO path on vmware/real hardware\n");
    }
    user_session_use_login();
    int initial_program_pid = create_user_process_from_file(initial_program.path, NULL, NULL);
    if (initial_program_pid < 0) shutdown_boot_failure("failed to create configured initial program");

    pcb_t initial_program_pcb = found_pcb(initial_program_pid);
    if (initial_program_pcb == NULL) shutdown_boot_failure("configured initial program was not published");
    initial_program_pcb->is_initial_program = true;

    enable_scheduler();
    open_interrupt;
    no_interrupt = false;

    while (true)
    {
        if (!no_interrupt)
        {
            enable_intr();
            enable_scheduler();
        }

        if (initial_program_pcb->status == ZOMBIE || initial_program_pcb->status == DEATH ||
            initial_program_pcb->status == OUT)
        {
            write_serial_fmt("BOOT: initial program pid=%d %s exit=%d; shutting down\n",
                             initial_program_pid,
                             initial_program_pcb->abnormal_exit ? "terminated abnormally" : "exited",
                             initial_program_pcb->exit_code);
            power_shutdown(EFI_ST, EFI_BC);
        }

        __asm__ __volatile__("pause");
    }
}
