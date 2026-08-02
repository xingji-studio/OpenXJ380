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
#include <pci/pci.h>
#include <pipe.h>
#include <power.h>
#include <proto.hpp>
#include <pty.h>
#include <ps2/keyboard.h>
#include <rtc.h>
#include <sb16.h>
#include <syscall/signal.h>
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

extern bool allow_to_flush;
extern void ahci_set_accel(bool enabled);
extern bool ahci_is_qemu_environment();
extern BOOT_CONFIG *EFI_BC;

extern UserInfo *current_user;

static const char *busybox_alias_applets[] = {
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
        for (int i = 0; busybox_alias_applets[i] != NULL; i++)
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

#if 0 // Legacy GUI session startup. Retained in source while excluded from the CLI kernel.
static uint8_t dispatch_keyboard_input_to_focused_window()
{
    uint8_t k_input = get_keyboard_input();
    if (k_input != NULL)
    {
        WINDOWLSP current_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
        if (current_window != NULL)
        {
            if (k_input != '\n' && k_input != '\b' && k_input < 128)
            {
                do_message(MSG_CHAR, NULL, k_input, current_window->WinMPf, current_window->w_task);
            }
            else
            {
                do_message(MSG_SPCHAR, NULL, k_input, current_window->WinMPf, current_window->w_task);
            }
        }
    }
    return k_input;
}

extern const uint8_t _binary___graphics_logo_xj380_2_png_start;
extern const uint8_t _binary___graphics_logo_xj380_2_png_end;

extern "C" unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y,
                                                 int *channels_in_file, int desired_channels);
extern "C" void           stbi_image_free(void *retval_from_stbi_load);
extern "C" int            stbir_resize_uint8(const unsigned char *input_pixels, int input_w, int input_h,
                                             int input_stride_in_bytes, unsigned char *output_pixels, int output_w,
                                              int output_h, int output_stride_in_bytes, int num_channels);
#endif

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

typedef struct TimerWidgetsData
{
    SHEET_INFO *sht;
    SHEET      *ct_sheet;
    tm          time;
    tm          old_time;
    int         scdx;
    uint8_t     flushing;
    uint8_t     need_flush;
} TimerWidgetsData;

TimerWidgetsData *tw_data = NULL;

bool have_full_screen_app = false;
extern bool user_dock_owns_dock_sheet;

int clock_hour_offset = 0;

void init_time()
{
    if (current_user == NULL) return;

    char setfile_path[256];
    memset(setfile_path, 0, 256);
    strcat(setfile_path, "/users/");
    strcat(setfile_path, current_user->name);
    strcat(setfile_path, "/settings.dat");
    vfs_node_t v = vfs_open(setfile_path);
    if (!v) return;

    SettingsDataFileFormat sdff;
    memset(&sdff, 0, sizeof(sdff));
    size_t read_size = v->size < sizeof(sdff) ? (size_t)v->size : sizeof(sdff);
    if (read_size == 0 || vfs_read(v, &sdff, 0, read_size) == -1)
    {
        vfs_close(v);
        return;
    }

    clock_hour_offset = sdff.ClockHourOffset;

    if (clock_hour_offset < 0)
        clock_hour_offset = 0;

    vfs_close(v);
}

#if 0 // Legacy framebuffer and desktop artwork.
static void draw_startup_screen(SHEET_INFO *shtinf, SHEET *desktop_sheet)
{
    if (shtinf == NULL || desktop_sheet == NULL) return;

    // 开机启动界面：纯色背景 + 居中 XJ380 Logo
    draw_rect(shtinf, desktop_sheet, 0, 0, shtinf->scrx - 1, shtinf->scry - 1, BLACK);

    int logo_w = (int)(shtinf->scrx * 0.26f);
    if (logo_w < 160) logo_w = 160;
    if (logo_w > 420) logo_w = 420;
    int logo_h = logo_w / 2;

    int logo_x = (shtinf->scrx - logo_w) / 2;
    int logo_y = (shtinf->scry - logo_h) / 2;

    // PrintPicture_blend(shtinf, desktop_sheet, logo_x, logo_y, logo_w, logo_h, (char *)"/system/xj380_2.png");

    refresh_sheet(shtinf);
    flush_sheet_damage_queue_now(shtinf);
}

static void draw_boot_progress_logo(const FrameBufferConfig &fbc)
{
    // 早期启动阶段（KernelMain 进度条）显示真实 XJ380 Logo
    rect(fbc, 0, 0, (int)fbc.horizontal_resolution, (int)fbc.vertical_resolution, {0x0d, 0x13, 0x24});

    const unsigned char *png      = (const unsigned char *)&_binary___graphics_logo_xj380_2_png_start;
    const unsigned char *png_end  = (const unsigned char *)&_binary___graphics_logo_xj380_2_png_end;
    size_t               png_bytes = (size_t)(png_end - png);
    if (png_bytes == 0) {
        WriteString(fbc, ((int)fbc.horizontal_resolution - 40) / 2, ((int)fbc.vertical_resolution - 16) / 2,
                    "XJ380", {0xff, 0xff, 0xff});
        return;
    }

    int src_w = 0, src_h = 0, src_c = 0;
    unsigned char *src_rgba = stbi_load_from_memory(png, (int)png_bytes, &src_w, &src_h, &src_c, 4);
    if (src_rgba == NULL || src_w <= 0 || src_h <= 0) {
        if (src_rgba) stbi_image_free(src_rgba);
        WriteString(fbc, ((int)fbc.horizontal_resolution - 40) / 2, ((int)fbc.vertical_resolution - 16) / 2,
                    "XJ380", {0xff, 0xff, 0xff});
        return;
    }

    int logo_w = (int)(fbc.horizontal_resolution * 0.30f);
    if (logo_w < 180) logo_w = 180;
    if (logo_w > 520) logo_w = 520;
    int logo_h = (int)(((int64_t)logo_w * src_h) / src_w);

    int logo_x = ((int)fbc.horizontal_resolution - logo_w) / 2;
    int logo_y = ((int)fbc.vertical_resolution - logo_h) / 2 - 8;
    if (logo_y < 16) logo_y = 16;

    unsigned char *blit_rgba = src_rgba;
    if (logo_w != src_w || logo_h != src_h) {
        unsigned char *scaled = (unsigned char *)malloc((size_t)logo_w * logo_h * 4);
        if (scaled != NULL &&
            stbir_resize_uint8(src_rgba, src_w, src_h, 0, scaled, logo_w, logo_h, 0, 4) != 0) {
            blit_rgba = scaled;
        } else if (scaled != NULL) {
            free(scaled);
            logo_w = src_w;
            logo_h = src_h;
            logo_x = ((int)fbc.horizontal_resolution - logo_w) / 2;
            logo_y = ((int)fbc.vertical_resolution - logo_h) / 2 - 8;
            if (logo_y < 16) logo_y = 16;
        }
    }

    for (int y = 0; y < logo_h; ++y)
    {
        int dst_y = logo_y + y;
        if (dst_y < 0 || dst_y >= (int)fbc.vertical_resolution) continue;
        for (int x = 0; x < logo_w; ++x)
        {
            int dst_x = logo_x + x;
            if (dst_x < 0 || dst_x >= (int)fbc.horizontal_resolution) continue;

            const uint8_t *src = &blit_rgba[(y * logo_w + x) * 4];
            uint8_t        sa  = src[3];
            if (sa == 0) continue;

            uint8_t *dst = PixelAt(dst_x, dst_y, fbc);
            uint8_t  dr, dg, db;
            if (fbc.pixel_format == PixelFormat::kRGBR) {
                dr = dst[0];
                dg = dst[1];
                db = dst[2];
            } else {
                db = dst[0];
                dg = dst[1];
                dr = dst[2];
            }

            uint8_t nr = (uint8_t)(((uint16_t)src[0] * sa + (uint16_t)dr * (255 - sa)) / 255);
            uint8_t ng = (uint8_t)(((uint16_t)src[1] * sa + (uint16_t)dg * (255 - sa)) / 255);
            uint8_t nb = (uint8_t)(((uint16_t)src[2] * sa + (uint16_t)db * (255 - sa)) / 255);

            if (fbc.pixel_format == PixelFormat::kRGBR) {
                dst[0] = nr;
                dst[1] = ng;
                dst[2] = nb;
            } else {
                dst[0] = nb;
                dst[1] = ng;
                dst[2] = nr;
            }
        }
    }

    if (blit_rgba != src_rgba) free(blit_rgba);
    stbi_image_free(src_rgba);

    WriteString(fbc, ((int)fbc.horizontal_resolution - 88) / 2, logo_y + logo_h + 14, "Starting...",
                {0xb4, 0xc2, 0xdd});
}
#endif

#if 0 // Legacy GUI compositor.
void components_flusher()
{
    while (true)
    {
        if (allow_to_flush && desktop_done) break;
        scheduler_yield();
    }
    write_serial_string("components flusher is running\n");
    while (true)
    {
        if (!no_interrupt)
        {
            enable_intr();
            enable_scheduler();
        }
        // if (tw_data != NULL && !have_full_screen_app && !user_dock_owns_dock_sheet)
        // {
        //     tm         *time          = &tw_data->time;
        //     tm         *old_time      = &tw_data->old_time;
        //     SHEET_INFO *shtinf        = tw_data->sht;
        //     SHEET      *dock_ct_sheet = tw_data->ct_sheet;
        //     int         scdx          = tw_data->scdx;
        //     time_read(time);

        //     if ((old_time->tm_min != time->tm_min || old_time->tm_hour != time->tm_hour) && !tw_data->need_flush &&
        //         !tw_data->flushing)
        //     {
        //         tw_data->flushing = true;
        //         *old_time         = *time;
        //         draw_rect(shtinf, dock_ct_sheet, scdx - 40, 0, scdx - 3, 23, DOCK_COL);
        //         // 12 小时制
        //         print_fmt_box_ttf(shtinf, dock_ct_sheet, BLACK, scdx - 40, 1, 10, "%02u:%02u", time->tm_hour % 24,
        //                           time->tm_min);
        //         tw_data->flushing   = false;
        //         tw_data->need_flush = true;
        //     }
        // }
        toast_manager_flush();
        if (ms_dec.need_flush || ms_dec.win_move_pending || ms_dec.win_resize_pending) { process_mouse_info(); }
        if (sht_img != NULL) { flush_sheet_damage_queue(sht_img); }
        scheduler_yield();
    }
}

extern SHEET *mouse_ct_sheet_img;
SHEET        *desktop_ct_sheet; // 如果SheetTest那行输出的是1145的话那么绝对有问题
SHEET        *dock_ct_sheet;
SHEET        *middle_ct_sheet;
SHEET        *mouse_ct_sheet;
SHEET        *top_ct_sheet;
SHEET *zhe_shi_yi_ge_sha_bi_dao_ji_zhi_de_cao_zuo_da_jia_bu_yao_xue___GuoqiFish_is_shabi_and_this_var_is_cao_gao_zhi;

void TEMP_stress_test_function();

static void halt_gui_startup_oom(const char *name)
{
    write_serial_string("GUI startup sheet allocation failed: ");
    write_serial_string(name);
    write_serial_string(". System halted.\n");
    while (true)
    {
        __asm__ __volatile__("hlt");
    }
}

void desktop_flusher()
{
    bool installer_mode = EFI_BC != NULL && installer_boot_active(*EFI_BC);
    no_interrupt = true;
    disable_intr();
    disable_scheduler();

    write_serial_string("desktop flusher is running\n");

    register_hardware_info();

    SHEET_INFO shtinf;
    init_sheet(*fbc_addr, &shtinf);

    sht_img = &shtinf;
    msid    = mouse_ct_sheet;

    if (!create_sheet(&shtinf, 0, 0, fbc_addr->horizontal_resolution, fbc_addr->vertical_resolution, FixedSheetType, 0,
                      &desktop_ct_sheet))
        halt_gui_startup_oom("desktop");
    if (!create_sheet(&shtinf, 0, 0, fbc_addr->horizontal_resolution, fbc_addr->vertical_resolution, FixedSheetType,
                      32700, &dock_ct_sheet))
        halt_gui_startup_oom("dock");
    if (!create_sheet(&shtinf, 0, 0, fbc_addr->horizontal_resolution, fbc_addr->vertical_resolution, FixedSheetType,
                      32759, &middle_ct_sheet))
        halt_gui_startup_oom("middle"); // 输入法类窗口放这层
    if (!create_sheet(&shtinf, (get_hor() - 16) / 2, (get_ver() - 28 - 29) / 2, 29, 22, FixedSheetType, 32766,
                      &mouse_ct_sheet))
        halt_gui_startup_oom("mouse");
    if (!create_sheet(&shtinf, 0, 0, fbc_addr->horizontal_resolution, fbc_addr->vertical_resolution, FixedSheetType,
                      32767, &top_ct_sheet))
        halt_gui_startup_oom("top");

    // 草稿纸吗，有点意思
    SHEET_INFO zhe_shi_yi_ge_sha_bi_dao_ji_zhi_de_cao_zuo_da_jia_bu_yao_xue___GuoqiFish_is_shabi_and_this_var_is_cao_gao_zhi_de_tu_ceng_xin_xi;
    init_sheet(*fbc_addr, &zhe_shi_yi_ge_sha_bi_dao_ji_zhi_de_cao_zuo_da_jia_bu_yao_xue___GuoqiFish_is_shabi_and_this_var_is_cao_gao_zhi_de_tu_ceng_xin_xi);
    if (!create_sheet(
            &zhe_shi_yi_ge_sha_bi_dao_ji_zhi_de_cao_zuo_da_jia_bu_yao_xue___GuoqiFish_is_shabi_and_this_var_is_cao_gao_zhi_de_tu_ceng_xin_xi,
            get_hor() + 114, get_ver() + 114, 1000, 1000, FixedSheetType, 1145,
            &zhe_shi_yi_ge_sha_bi_dao_ji_zhi_de_cao_zuo_da_jia_bu_yao_xue___GuoqiFish_is_shabi_and_this_var_is_cao_gao_zhi))
        halt_gui_startup_oom("scratch");

    init_ttf();
    draw_startup_screen(&shtinf, desktop_ct_sheet);

    rect(*fbc_addr, 0, 0, int(fbc_addr->horizontal_resolution) / 10 * 9, 7, {0x00, 0xa2, 0xe8}); // GUI好了来一下

    int fontx = 8;
    int fonty = 50;
    // DEBUG
#if 1 == 2
    // #if BUILD_EDITION == DEBUG_VERSION
    PrintString(&shtinf, &desktop_ct_sheet, fbc_addr->horizontal_resolution - 64, fbc_addr->vertical_resolution - 40,
                "X:", WHITE);
    PrintString(&shtinf, &desktop_ct_sheet, fbc_addr->horizontal_resolution - 64, fbc_addr->vertical_resolution - 24,
                "Y:", WHITE);
    PrintDec(&shtinf, &desktop_ct_sheet, fbc_addr->horizontal_resolution - 48, fbc_addr->vertical_resolution - 40,
             shtinf.scrx, WHITE);
    PrintDec(&shtinf, &desktop_ct_sheet, fbc_addr->horizontal_resolution - 48, fbc_addr->vertical_resolution - 24,
             shtinf.scry, WHITE);
    PrintString(&shtinf, &desktop_ct_sheet, 8, 34, "KeyboardInputOut", WHITE);
    // PrintString(&shtinf, &desktop_ct_sheet, 332, 34, "PointerInputOut", WHITE);
    // PrintString(&shtinf, &desktop_ct_sheet, 8, 480, "MapKey:", WHITE);
    // PrintHex(&shtinf, &desktop_ct_sheet, 8, 496, BC->MemoryMap.MapKey, WHITE);
    // 别问为什么这么多-40，问就是因为改起来太麻烦
    PrintString(&shtinf, &desktop_ct_sheet, 8, 284, "PrintTestColor", WHITE);

    draw_rect(&shtinf, &desktop_ct_sheet, 480 - 472, 300, 500 - 473, 319, WHITE);
    draw_rect(&shtinf, &desktop_ct_sheet, 500 - 472, 300, 520 - 473, 319, BLACK);
    draw_rect(&shtinf, &desktop_ct_sheet, 520 - 472, 300, 540 - 473, 319, XINGJI_BLUE);
    draw_rect(&shtinf, &desktop_ct_sheet, 540 - 472, 300, 560 - 473, 319, XINGJI_YELLOW);
    draw_rect(&shtinf, &desktop_ct_sheet, 560 - 472, 300, 580 - 473, 319, WIN_BLUE);
    draw_rect(&shtinf, &desktop_ct_sheet, 580 - 472, 300, 600 - 473, 319, GRAY);
    draw_rect(&shtinf, &desktop_ct_sheet, 600 - 472, 300, 620 - 473, 319, BGRAY);
    draw_rect(&shtinf, &desktop_ct_sheet, 620 - 472, 300, 640 - 473, 319, PURPLE);
    draw_rect(&shtinf, &desktop_ct_sheet, 640 - 472, 300, 660 - 473, 319, PINK);
    draw_rect(&shtinf, &desktop_ct_sheet, 660 - 472, 300, 680 - 473, 319, RED);
    draw_rect(&shtinf, &desktop_ct_sheet, 680 - 472, 300, 700 - 473, 319, CHINA_RED);
    draw_rect(&shtinf, &desktop_ct_sheet, 700 - 472, 300, 720 - 473, 319, GOLD);
    draw_rect(&shtinf, &desktop_ct_sheet, 720 - 472, 300, 740 - 473, 319, GREEN);
    draw_rect(&shtinf, &desktop_ct_sheet, 740 - 472, 300, 760 - 473, 319, LIGHT_GREEN);
    draw_rect(&shtinf, &desktop_ct_sheet, 760 - 472, 300, 780 - 473, 319, BLUE);
    draw_rect(&shtinf, &desktop_ct_sheet, 780 - 472, 300, 800 - 473, 319, SKY_BLUE);

    PrintString(&shtinf, &desktop_ct_sheet, 8, 332, "MemoryMapTypeSheet", WHITE);
    PrintString(&shtinf, &desktop_ct_sheet, 8, 348, "0x0: Free", WHITE);
    PrintString(&shtinf, &desktop_ct_sheet, 8, 364, "0xB: EFI Code & Data", WHITE);

    PrintString(&shtinf, &desktop_ct_sheet, 8, 400, "SheetManagerTest", WHITE);
    PrintDec(&shtinf, &desktop_ct_sheet, 8, 416, (uint64_t)desktop_ct_sheet, WHITE);
    // PrintDec(&shtinf, &desktop_ct_sheet, 164, 416, (uint64_t)*shtinf.sheet[0].number, WHITE);

    PrintHex(&shtinf, &desktop_ct_sheet, 8, 432, (uint64_t)shtinf.temp_buffer, WHITE);
    PrintDec(&shtinf, &desktop_ct_sheet, 164, 432, (uint64_t)shtinf.sheet_num, WHITE);

    PrintHex(&shtinf, &desktop_ct_sheet, 8, 448, 0xFEDCBA9876543210, WHITE);
    // PrintHex(&shtinf, &desktop_ct_sheet, 164, 448, (uint64_t)shtinf.sheet[255].buffer, WHITE);

    // CPU型号
    char CPU_Model[48];
    get_cpu_name(CPU_Model);
    PrintString(&shtinf, &desktop_ct_sheet, 8, 528, "CPU Model:", WHITE);
    PrintString(&shtinf, &desktop_ct_sheet, 96, 528, CPU_Model, WHITE);
    write_serial_string("CPU Model: ");
    write_serial_string(CPU_Model);
    write_serial_string("\n");

    print_fmt_box_ttf(&shtinf, &desktop_ct_sheet, WHITE, 64, 64, 16,
                      "MAKE \nXJ380 \nGREAT \nAGAIN! \n实现XJ380的伟大复兴！");
#endif
    // wsod_debug();

    // print_box_ttf(&shtinf, &desktop_ct_sheet, (char *)"MAKE XJ380 GREAT AGAIN! 实现XJ380的伟大复兴！", WHITE, 64, 64,
    //               16);

    draw_mouse(&shtinf, mouse_ct_sheet);
    XWM_INFO  xwmi;
    WINDOWLS *test_win;
    WINDOWLS *test_win2;
    init_xwm(&xwmi);
    xwmii = &xwmi;

    rect(*fbc_addr, 0, 0, int(fbc_addr->horizontal_resolution) / 10 * 10, 7, {0x00, 0xa2, 0xe8}); // 全都好了来一下

    mouse_ct_sheet_img = mouse_ct_sheet;

    allow_to_flush = true;
    no_interrupt = false;
    enable_intr();
    enable_scheduler();

    if (!installer_mode) {
        write_serial_string("desktop: prepare login session\n");
        user_session_use_root();
        write_serial_string("desktop: init_xuls begin\n");
        init_xuls();
        write_serial_string("desktop: init_xuls done\n");

        write_serial_string("user & settings initialized.\n");

        write_serial_string("desktop initialized.\n");
    } else {
        write_serial_string("installer: skipping normal desktop login\n");
        draw_rect(&shtinf, desktop_ct_sheet, 0, 0, shtinf.scrx - 1, shtinf.scry - 1, {0x0f, 0x4c, 0x9a, 0xff});
    }

    winRD_lock = true;

    refresh_sheet(&shtinf); // 刷新
    flush_sheet_damage_queue_now(&shtinf);

    uint8_t k_input;

    // struct EFI_RUNTIME_SERVICES *RuntimeServices = (struct EFI_RUNTIME_SERVICES *)convert_physical_to_virtual((uint64_t)(ST->RuntimeServices));
    // EFIAPI EFI_STATUS (*GetTime)(EFI_TIME *, EFI_TIME_CAPABILITIES *) = (EFIAPI EFI_STATUS(*)(EFI_TIME *, EFI_TIME_CAPABILITIES *))convert_physical_to_virtual((uint64_t)RuntimeServices->GetTime);
    int scdx = fbc_addr->horizontal_resolution * 13 / 16;

    desktop_done = true;

    delay_s_hp(1);



    // char *argv[2] = {"/system/icon/folder.png", NULL};
    // create_user_process_from_file((char *)"/apps/init.elf", NULL, NULL);
// #if defined(CONFIG_KERNEL_AUTO_START_USER_APP)
//     create_user_process_from_file((char *)CONFIG_KERNEL_DEFAULT_USER_APP, NULL, NULL);
// #endif
    // wav_player("/system/test.wav");
    // create_user_process_from_file((char *)"/system/picturer.elf", NULL, argv);
    if (installer_mode) {
        installer_launch_app();
    } else {
        create_user_process_from_file((char *)"/apps/system/login.elf", NULL, NULL);
        while (current_user == NULL || current_user->user_type == XUT_Root)
        {
            dispatch_keyboard_input_to_focused_window();
            scheduler_yield();
            flush_sheet_damage_queue_now(&shtinf);
        }
        init_time();
        create_user_process_from_file((char *)"/apps/system/desktop.elf", NULL, NULL);
        for (int i = 0; i < 8; i++)
        {
            scheduler_yield();
            flush_sheet_damage_queue_now(&shtinf);
        }
        create_user_process_from_file((char *)"/apps/system/dock.elf", NULL, NULL);

        // TEMP_stress_test_function();
    }
    // create_user_process_from_file((char *)"/system/ctrlmenu.elf", NULL, NULL);
    // create_user_process_from_file((char *)"/apps/cp537.elf", NULL, NULL);
    // create_user_process_from_file((char *)"/apps/bcms-sp.elf", NULL, NULL);

    while (1)
    {
        if (!no_interrupt)
        {
            enable_intr();
            enable_scheduler();
        }
        // if (tw_data != NULL && tw_data->need_flush && !user_dock_owns_dock_sheet)
        // {
        //     tw_data->need_flush = false;
        //     refresh_part_sheet(&shtinf, scdx - 40, 0, scdx, 24); // 刷新时间
        // }

        k_input = dispatch_keyboard_input_to_focused_window();

#if BUILD_EDITION == DEBUG_VERSION
        if (k_input != NULL && k_input != '\n' && k_input != '\b')
        {
            PrintFont(&shtinf, desktop_ct_sheet, fontx, fonty, k_input, WHITE);
            refresh_part_sheet(&shtinf, fontx, fonty, fontx + 8, fonty + 16); // 刷新
            fontx += 8;
        }
        else if (k_input == '\n')
        {
            fontx  = 8;
            fonty += 16;
        }
        if (fontx > 304)
        {
            fontx  = 8;
            fonty += 16;
        }
#endif
        flush_sheet_damage_queue(&shtinf);
        scheduler_yield();
    }
}
#endif

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
#if CONFIG_KERNEL_BUILTIN_XHCI
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
#if CONFIG_KERNEL_BUILTIN_XHCI
    write_serial_string("BOOT: xhci_setup begin\n");
    xhci_setup();
    write_serial_string("BOOT: xhci_setup done\n");
#endif
    write_serial_string("BOOT: partition_init begin\n");
    partition_init();
    write_serial_string("BOOT: partition_init done\n");
    // enable_intr();

    // disable_intr();
    // HDA 驱动现在会在初始化阶段自行完成注册，这里只需要启动探测即可。
    hda_init();
    hda_regist();
    // sb16_init();

    keyboard_init();

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

#if CONFIG_KERNEL_BUILTIN_XHCI
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

    if ((BootConfig.boot_flags & (BOOT_FLAG_SAFE_MODE | BOOT_FLAG_DISABLE_KMOD | BOOT_FLAG_INSTALLER)) == 0) {
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
