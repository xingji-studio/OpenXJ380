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
#include <hda/pcspk.h>
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
#include <sb16.h>
#include <syscall/signal.h>
#include <syscall/pxapi.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/runfile.h>
#include <user/settings.h>
#include <user/info_register.h>
#include <hda/vsound.h>
#include <hda/hda.h>
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
extern BOOT_CONFIG *EFI_BC;

extern UserInfo *current_user;

static char busybox_alias_applets[][16] = {
    "[",       "[[",      "ash",      "awk",      "basename", "cat",      "chmod",   "chgrp",
    "chown",   "clear",   "cmp",      "cp",       "cut",      "date",     "dd",      "df",
    "dirname", "dmesg",   "du",       "echo",     "egrep",    "env",      "false",   "fgrep",
    "find",    "free",    "grep",     "gunzip",   "gzip",     "head",     "hexdump", "hostname",
    "id",      "ifconfig","install",  "ip",       "kill",     "killall",  "less",    "ln",
    "ls",
    "mkdir",   "more",    "mount",    "mv",       "nc",       "netstat",  "nslookup","od",
    "pgrep",   "pidof",   "ping",     "pkill",    "printenv", "printf",   "ps",      "pwd",
    "readlink","realpath","reset",    "rm",       "rmdir",    "route",    "sed",     "sh",
    "sleep",   "sort",    "stat",     "stty",     "sync",     "tail",     "tar",     "test",
    "top",     "touch",   "tr",       "true",     "tty",      "umount",   "uname",   "uniq",
    "unzip",   "uptime",  "usleep",   "vi",       "wc",       "which",   "whoami",
    "xargs",   "xxd",     "zcat",     NULL,
};//暴力枚举这一块，好像只能这么做了
  //Maybe we can try to load this applet when vfs inited.

static void load_busybox_alias_applets()
{
    if (current_user == NULL) return;

    char setfile_path[256];
    memset(setfile_path, 0, 256);
    strcat(setfile_path, "/etc/busybox/alias/applets.dat");
    vfs_node_t vfp = vfs_open(setfile_path);
    if (!vfp) return;
    char tmp[1024];
    if (vfp->size >= sizeof(tmp))
    {
        vfs_close(vfp);
        return;
    }
    vfs_read(vfp, tmp, 0, vfp->size);
    char alias[8];
    memset(alias, 0, 8);
    int applet_index = 0, alias_index = 0;
    const int applet_count = sizeof(busybox_alias_applets) / sizeof(busybox_alias_applets[0]);
    for (uint64_t i = 0; i < vfp->size && applet_index < applet_count - 1; i++)
    {
        if (tmp[i] == ',')
        {
            strcpy(busybox_alias_applets[applet_index], alias);
            applet_index++;
            alias_index = 0;
            memset(alias, 0, sizeof(alias));
            continue;
        }
        if (alias_index < sizeof(alias) - 1) alias[alias_index++] = tmp[i];
    }
    if (alias_index > 0 && applet_index < applet_count - 1) strcpy(busybox_alias_applets[applet_index], alias);

    vfs_close(vfp);
}

static const char *busybox_binary_path = "/apps/busybox";

static void setup_xbps_vfs_aliases()
{
    static const char *xbps_void_key_alias =
        "/var/db/xbps/keys/60:ae:0c:d6:f0:95:17:80:bc:93:46:7a:89:af:a3:2d.plist";
    static const char *xbps_void_key_target =
        "/var/db/xbps/keys/60_ae_0c_d6_f0_95_17_80_bc_93_46_7a_89_af_a3_2d.plist";

    if (vfs_register_alias(xbps_void_key_alias, xbps_void_key_target) == EOK)
    {
        write_serial_fmt("[xbps-debug] key alias %s -> %s\n", xbps_void_key_alias, xbps_void_key_target);
    }
}

static void setup_busybox_vfs_aliases()
{
    const char *prefixes[] = {"/apps", "/bin", NULL};
    for (int p = 0; prefixes[p] != NULL; p++)
    {
        for (int i = 0; busybox_alias_applets[i][0] != '\0'; i++)
        {
            char alias_path[128];
            snprintf(alias_path, sizeof(alias_path), "%s/%s", prefixes[p], busybox_alias_applets[i]);
            if (vfs_register_alias(alias_path, busybox_binary_path) == EOK)
            {
                write_serial_fmt("[busybox-debug] busybox alias %s -> %s\n", alias_path, busybox_binary_path);
            }
        }
    }
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
EFI_SYSTEM_TABLE *EFI_ST;
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
    // enable_intr();

    // disable_intr();
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    // HDA 驱动现在会在初始化阶段自行完成注册，这里只需要启动探测即可。
    hda_init();
    hda_regist();
#endif
    // sb16_init();

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

    bool installer_mode = false;
    mount_root();
#if CONFIG_KERNEL_BUSYBOX_ALIASES
    setup_busybox_vfs_aliases();
#endif
    setup_xbps_vfs_aliases();
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

    while (true)
    {
        __asm__ volatile("pause");
        if (scheduler_is_ready == xsi->cpu_count) break;
    }

    init_reaper();
    write_serial_string("Process Reaper is ready\n");

    if (BootConfig.is_qemu == 1) pr_warn("XJ380 is running under the QEMU/BOCHS.\n");

    // create_kernel_thread((void *)tst, NULL, (char *)"1", NULL);
    if ((BootConfig.boot_flags & (BOOT_FLAG_SAFE_MODE | BOOT_FLAG_SAFE_STORAGE_IO)) != 0) {
        write_serial_string("AHCI: keeping conservative IO path by boot menu\n");
    } else if (ahci_is_qemu_environment()) {
        ahci_set_accel(true);
    } else {
        write_serial_string("AHCI: keeping conservative IO path on vmware/real hardware\n");
    }
    user_session_use_login();
    create_user_process_from_file((char *)"/apps/system/shell.elf", NULL, NULL);

    // delay_s_hp(60);

    // uint64_t utsk = page_alloc_random(get_current_directory(), 114, PTE_PRESENT | PTE_USER);
    // memcpy((void *)utsk, (void *)test_task, 114);
    // create_user_thread((void *)utsk, NULL, (char *)"test_task2", ugp);


    enable_scheduler();
    open_interrupt;
    no_interrupt = false;

    // no_interrupt = true;
    // close_interrupt;
    // disable_scheduler();

    // enable_scheduler();
    // open_interrupt;
    // no_interrupt = false;

    while (true)
    {
        if (!no_interrupt)
        {
            enable_intr();
            enable_scheduler();
        }

        __asm__ __volatile__("pause");
    }
}
