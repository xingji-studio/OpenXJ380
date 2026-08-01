#include <krlibc.h>
#include <x3api.h>
#include <installer_protocol.h>
#include <xj380_i18n.h>
#include "installer_hidden.h"

#define COLOR_BG          0x0B3D91FF
#define COLOR_BG_2        0x0F5FB8FF
#define COLOR_PANEL       0xF7FBFFFF
#define COLOR_PANEL_SOFT  0xEAF4FFFF
#define COLOR_PANEL_LINE  0x9CC7F4FF
#define COLOR_TEXT        0x102A43FF
#define COLOR_MUTED       0x526A83FF
#define COLOR_WHITE       0xFFFFFFFF
#define COLOR_BLUE        0x136FDCFF
#define COLOR_BLUE_DARK   0x0B4AA2FF
#define COLOR_GREEN       0x1EAD63FF
#define COLOR_RED         0xD64545FF
#define COLOR_YELLOW      0xB7791FFF
#define COLOR_DISABLED    0x94A3B8FF

enum InstallerPage
{
    PAGE_HOME = 0,
    PAGE_CONFIRM,
    PAGE_PROGRESS,
    PAGE_RESCUE,
    PAGE_LOG,
};

static HDLE g_window = 0;
static UINT64 g_width = 1024;
static UINT64 g_height = 700;
static bool g_need_redraw = true;
static bool g_install_requested = false;
static int g_selected = 0;
static int g_language = XJ380_LANGUAGE_ZH_CN;
static UINT32 g_mode = XJ380_INSTALLER_MODE_FRESH;
static UINT64 g_components = XJ380_INSTALLER_COMPONENT_DEFAULT;
static InstallerPage g_page = PAGE_HOME;
static xj380_installer_disk_list g_disks;
static xj380_installer_precheck g_precheck;
static xj380_installer_progress g_progress;
static xj380_installer_rescue_result g_rescue;
static xj380_installer_log g_log;
static bool g_window_closed_for_terminal = false;

static char *inst_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static const char *mode_name(UINT32 mode)
{
    switch (mode)
    {
    case XJ380_INSTALLER_MODE_REPAIR_BOOT: return inst_tr("仅修复引导", "Repair boot only");
    case XJ380_INSTALLER_MODE_KEEP_USERS: return inst_tr("重装系统保留用户", "Reinstall and keep users");
    case XJ380_INSTALLER_MODE_DEVELOPER: return inst_tr("开发者安装", "Developer install");
    case XJ380_INSTALLER_MODE_FRESH:
    default: return inst_tr("全新安装", "Fresh install");
    }
}

static const char *mode_detail(UINT32 mode)
{
    switch (mode)
    {
    case XJ380_INSTALLER_MODE_REPAIR_BOOT:
        return inst_tr("只重写 EFI 引导文件和系统内核。", "Rewrite only EFI boot files and the system kernel.");
    case XJ380_INSTALLER_MODE_KEEP_USERS:
        return inst_tr("重装系统文件，跳过 /users 目录。", "Reinstall system files and skip /users.");
    case XJ380_INSTALLER_MODE_DEVELOPER:
        return inst_tr("全新安装，并显示详细日志。", "Fresh install with detailed logs.");
    case XJ380_INSTALLER_MODE_FRESH:
    default: return inst_tr("清空目标硬盘并安装完整系统。", "Erase the target disk and install the full system.");
    }
}

static UINT64 normalize_components(UINT64 components)
{
    components &= XJ380_INSTALLER_COMPONENT_ALL;
    if ((components & XJ380_INSTALLER_COMPONENT_LINUX_COMPAT) == 0)
        components = 0;
    return components;
}

static void append_component_text(char *out, UINT64 out_size, const char *text, bool *first)
{
    if (out == NULL || out_size == 0 || text == NULL || first == NULL) return;
    UINT64 len = strlen(out);
    if (len >= out_size - 1) return;
    if (!*first)
    {
        snprintf(out + len, out_size - len, "%s", inst_tr("、", ", "));
        len = strlen(out);
        if (len >= out_size - 1) return;
    }
    snprintf(out + len, out_size - len, "%s", text);
    *first = false;
}

static void components_to_text(char *out, UINT64 out_size)
{
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    UINT64 components = normalize_components(g_components);
    if ((components & XJ380_INSTALLER_COMPONENT_LINUX_COMPAT) == 0)
    {
        snprintf(out, out_size, "%s", inst_tr("不安装 Linux 兼容层", "Do not install Linux compatibility layer"));
        return;
    }

    bool first = true;
    append_component_text(out, out_size, inst_tr("Linux 兼容层基础环境", "Linux compatibility base"), &first);
    if ((components & XJ380_INSTALLER_COMPONENT_PYTHON) != 0)
        append_component_text(out, out_size, "Python", &first);
    if ((components & XJ380_INSTALLER_COMPONENT_LLVM_CLANG) != 0)
        append_component_text(out, out_size, "LLVM / Clang", &first);
    if ((components & XJ380_INSTALLER_COMPONENT_GCC) != 0)
        append_component_text(out, out_size, "GCC", &first);
}

static void set_local_failure(INT64 result, const char *stage, const char *detail)
{
    memset(&g_progress, 0, sizeof(g_progress));
    g_progress.state = XJ380_INSTALLER_FAILED;
    g_progress.percent = 0;
    g_progress.result = result;
    if (stage != NULL) strncpy(g_progress.stage, stage, sizeof(g_progress.stage) - 1);
    if (detail != NULL) strncpy(g_progress.detail, detail, sizeof(g_progress.detail) - 1);
    g_page = PAGE_PROGRESS;
    g_need_redraw = true;
}

static bool progress_equal(const xj380_installer_progress *a, const xj380_installer_progress *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static bool in_rect(UINT64 x, UINT64 y, int x1, int y1, int x2, int y2)
{
    return x >= (UINT64)x1 && x <= (UINT64)x2 && y >= (UINT64)y1 && y <= (UINT64)y2;
}

static void bytes_to_text(UINT64 bytes, char *out, UINT64 out_size)
{
    if (out == NULL || out_size == 0) return;
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
    {
        UINT64 gb10 = (bytes * 10ULL) / (1024ULL * 1024ULL * 1024ULL);
        snprintf(out, out_size, "%llu.%llu GiB", gb10 / 10ULL, gb10 % 10ULL);
    }
    else if (bytes >= 1024ULL * 1024ULL)
    {
        UINT64 mb = bytes / (1024ULL * 1024ULL);
        snprintf(out, out_size, "%llu MiB", mb);
    }
    else
    {
        UINT64 kb = bytes / 1024ULL;
        snprintf(out, out_size, "%llu KiB", kb);
    }
}

static void speed_to_text(UINT64 bytes_per_second, char *out, UINT64 out_size)
{
    if (out == NULL || out_size == 0) return;
    if (bytes_per_second == 0)
    {
        strcpy(out, inst_tr("-- MB/s", "-- MB/s"));
        return;
    }
    UINT64 mb10 = (bytes_per_second * 10ULL) / (1024ULL * 1024ULL);
    snprintf(out, out_size, "%llu.%llu MB/s", mb10 / 10ULL, mb10 % 10ULL);
}

static void seconds_to_text(UINT64 seconds, char *out, UINT64 out_size)
{
    if (out == NULL || out_size == 0) return;
    if (seconds == 0)
    {
        strcpy(out, inst_tr("--", "--"));
        return;
    }
    snprintf(out, out_size, "%llu:%02llu", seconds / 60ULL, seconds % 60ULL);
}

static int disk_row_at(UINT64 y)
{
    int start_y = 198;
    int row_h = 54;
    if (y < (UINT64)start_y) return -1;
    int idx = (int)((y - start_y) / row_h);
    if (idx < 0 || idx >= (int)g_disks.count) return -1;
    return idx;
}

static void run_precheck();

static void draw_button(int x1, int y1, int x2, int y2, const char *text, UINT32 color)
{
    xapi_DrawRect(g_window, x1, y1, x2, y2, color, true);
    xapi_DrawRect(g_window, x1, y1, x2, y2, 0x00000028, false);
    xapi_DrawSWText(g_window, x1 + 16, y1 + 10, (char *)text, COLOR_WHITE);
}

static int language_selector_x()
{
    int x = (int)g_width - 620;
    return x < 310 ? 310 : x;
}

static bool handle_language_click(UINT64 x, UINT64 y)
{
    if (g_install_requested) return false;

    int lang_x = language_selector_x();
    int next_language = -1;
    if (in_rect(x, y, lang_x + 56, 28, lang_x + 128, 64))
        next_language = XJ380_LANGUAGE_ZH_CN;
    else if (in_rect(x, y, lang_x + 136, 28, lang_x + 232, 64))
        next_language = XJ380_LANGUAGE_EN_US;
    else
        return false;

    g_language = xj380_normalize_language(next_language);
    run_precheck();
    g_need_redraw = true;
    return true;
}

static void draw_language_selector()
{
    int lang_x = language_selector_x();
    UINT32 zh_color = g_language == XJ380_LANGUAGE_ZH_CN ? COLOR_BLUE_DARK : COLOR_BLUE;
    UINT32 en_color = g_language == XJ380_LANGUAGE_EN_US ? COLOR_BLUE_DARK : COLOR_BLUE;
    if (g_install_requested)
    {
        zh_color = g_language == XJ380_LANGUAGE_ZH_CN ? COLOR_BLUE_DARK : COLOR_DISABLED;
        en_color = g_language == XJ380_LANGUAGE_EN_US ? COLOR_BLUE_DARK : COLOR_DISABLED;
    }
    xapi_DrawSWText(g_window, lang_x, 38, inst_tr("语言", "Language"), 0xE7F3FFFF);
    draw_button(lang_x + 56, 28, lang_x + 128, 64, "中文", zh_color);
    draw_button(lang_x + 136, 28, lang_x + 232, 64, "English", en_color);
}

static void draw_progress_bar(int x1, int y1, int x2, int y2, UINT32 percent)
{
    if (percent > 100) percent = 100;
    xapi_DrawRect(g_window, x1, y1, x2, y2, 0xDCEBFAFF, true);
    xapi_DrawRect(g_window, x1, y1, x2, y2, COLOR_PANEL_LINE, false);
    int fill = x1 + (int)(((UINT64)(x2 - x1) * percent) / 100ULL);
    if (fill > x1) xapi_DrawRect(g_window, x1, y1, fill, y2, COLOR_BLUE, true);
}

static void shorten_path(const char *src, char *dst, UINT64 dst_size)
{
    if (dst == NULL || dst_size == 0) return;
    dst[0] = '\0';
    if (src == NULL || src[0] == '\0') return;
    size_t len = strlen(src);
    if (len < dst_size)
    {
        strncpy(dst, src, (size_t)dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }
    if (dst_size <= 8)
    {
        strncpy(dst, src, (size_t)dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }
    size_t head = (size_t)((dst_size - 4) / 2);
    size_t tail = (size_t)dst_size - 4 - head;
    snprintf(dst, (size_t)dst_size, "%.*s...%.*s", (int)head, src, (int)tail, src + len - tail);
}

static void refresh_disks()
{
    memset(&g_disks, 0, sizeof(g_disks));
    xapi_InstallerEnumDisks(&g_disks);
    if (g_selected >= (int)g_disks.count) g_selected = g_disks.count > 0 ? 0 : -1;
    g_need_redraw = true;
}

static void run_precheck()
{
    memset(&g_precheck, 0, sizeof(g_precheck));
    if (g_selected < 0 || g_selected >= (int)g_disks.count)
    {
        g_precheck.can_continue = 0;
        return;
    }
    xj380_installer_start_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.disk_id = g_disks.disks[g_selected].id;
    opts.mode = g_mode;
    opts.language = (UINT32)xj380_normalize_language(g_language);
    opts.components = normalize_components(g_components);
    xapi_InstallerPrecheckOptions(&opts, &g_precheck);
}

static void draw_header(const char *subtitle)
{
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, COLOR_BG, true);
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, 92, COLOR_BG_2, true);
    xapi_DrawSWText(g_window, 30, 22, inst_tr("XJ380 安装程序", "XJ380 Installer"), COLOR_WHITE);
    xapi_DrawSWText(g_window, 30, 56, (char *)subtitle, 0xE7F3FFFF);
    draw_language_selector();

    int right = (int)g_width - 360;
    draw_button(right, 28, right + 96, 64, inst_tr("主页", "Home"),
                g_page == PAGE_HOME ? COLOR_BLUE_DARK : COLOR_BLUE);
    draw_button(right + 108, 28, right + 204, 64, inst_tr("救援", "Rescue"),
                g_page == PAGE_RESCUE ? COLOR_BLUE_DARK : COLOR_BLUE);
    draw_button(right + 216, 28, right + 312, 64, inst_tr("日志", "Log"),
                g_page == PAGE_LOG ? COLOR_BLUE_DARK : COLOR_BLUE);
}

static void draw_mode_cards(int x, int y, int w)
{
    xapi_DrawSWText(g_window, x, y, inst_tr("安装模式", "Install mode"), COLOR_TEXT);
    for (int i = 0; i < 4; i++)
    {
        int row_y = y + 30 + i * 58;
        UINT32 bg = ((UINT32)i == g_mode) ? 0xD9ECFFFF : COLOR_WHITE;
        xapi_DrawRect(g_window, x, row_y, x + w, row_y + 46, bg, true);
        xapi_DrawRect(g_window, x, row_y, x + w, row_y + 46, COLOR_PANEL_LINE, false);
        xapi_DrawSWText(g_window, x + 14, row_y + 8, (char *)mode_name((UINT32)i), COLOR_BLUE_DARK);
        int detail_x = g_language == XJ380_LANGUAGE_EN_US ? x + 250 : x + 156;
        xapi_DrawSWText(g_window, detail_x, row_y + 8, (char *)mode_detail((UINT32)i), COLOR_MUTED);
    }
}

static void draw_component_row(int x, int y, int w, const char *name, const char *detail,
                               bool checked, bool enabled, int indent)
{
    UINT32 text_color = enabled ? COLOR_TEXT : COLOR_DISABLED;
    UINT32 detail_color = enabled ? COLOR_MUTED : COLOR_DISABLED;
    UINT32 box_color = enabled ? COLOR_BLUE_DARK : COLOR_DISABLED;
    int row_x = x + indent;
    xapi_DrawRect(g_window, row_x, y + 5, row_x + 16, y + 21, enabled ? COLOR_WHITE : 0xE2E8F0FF, true);
    xapi_DrawRect(g_window, row_x, y + 5, row_x + 16, y + 21, box_color, false);
    if (checked)
    {
        xapi_DrawRect(g_window, row_x + 4, y + 9, row_x + 12, y + 17, box_color, true);
    }
    xapi_DrawSWText(g_window, row_x + 28, y + 2, (char *)name, text_color);
    xapi_DrawSWText(g_window, x + w - 250, y + 2, (char *)detail, detail_color);
}

static int components_panel_y()
{
    return 118 + 18 + 30 + 4 * 58 + 18;
}

static int component_row_at(UINT64 y)
{
    int panel_y = components_panel_y();
    int row_y = panel_y + 32;
    if (y < (UINT64)row_y) return -1;
    int row = (int)((y - row_y) / 28);
    if (row < 0 || row > 3) return -1;
    return row;
}

static void toggle_component_row(int row)
{
    g_components = normalize_components(g_components);
    if (row == 0)
    {
        if ((g_components & XJ380_INSTALLER_COMPONENT_LINUX_COMPAT) != 0)
            g_components = 0;
        else
            g_components = XJ380_INSTALLER_COMPONENT_DEFAULT;
        return;
    }

    if ((g_components & XJ380_INSTALLER_COMPONENT_LINUX_COMPAT) == 0)
        return;

    if (row == 1)
        g_components ^= XJ380_INSTALLER_COMPONENT_PYTHON;
    else if (row == 2)
        g_components ^= XJ380_INSTALLER_COMPONENT_LLVM_CLANG;
    else if (row == 3)
        g_components ^= XJ380_INSTALLER_COMPONENT_GCC;
    g_components = normalize_components(g_components);
}

static void draw_component_tree(int x, int y, int w)
{
    xapi_DrawSWText(g_window, x, y, inst_tr("可选组件", "Optional components"), COLOR_TEXT);
    xapi_DrawSWText(g_window, x + 128, y,
                    inst_tr("取消勾选后不会复制对应目录和工具链文件。",
                            "Unchecked packages will not be copied."),
                    COLOR_MUTED);

    UINT64 components = normalize_components(g_components);
    bool linux_on = (components & XJ380_INSTALLER_COMPONENT_LINUX_COMPAT) != 0;
    int row_y = y + 32;
    draw_component_row(x, row_y, w, inst_tr("Linux 兼容层", "Linux compatibility"),
                       inst_tr("/usr、动态库、Linux 工具基础环境", "/usr, shared libraries, Linux tools"),
                       linux_on, true, 0);
    draw_component_row(x, row_y + 28, w, "└ Python",
                       inst_tr("/usr 中的 Python 运行库和命令", "Python runtime and commands in /usr"),
                       linux_on && (components & XJ380_INSTALLER_COMPONENT_PYTHON) != 0, linux_on, 20);
    draw_component_row(x, row_y + 56, w, "└ LLVM / Clang",
                       inst_tr("clang、llvm、lld 相关文件", "clang, llvm, and lld files"),
                       linux_on && (components & XJ380_INSTALLER_COMPONENT_LLVM_CLANG) != 0, linux_on, 20);
    draw_component_row(x, row_y + 84, w, "└ GCC",
                       inst_tr("gcc、g++、cpp、libgcc 和 C++ 头文件",
                               "gcc, g++, cpp, libgcc, and C++ headers"),
                       linux_on && (components & XJ380_INSTALLER_COMPONENT_GCC) != 0, linux_on, 20);
}

static void draw_disk_rows(int x, int y, int w)
{
    if (g_disks.count == 0)
    {
        xapi_DrawSWText(g_window, x + 20, y + 20,
                        inst_tr("未发现可安装的硬盘。请确认 QEMU 已挂载目标磁盘。",
                                "No installable disk was found. Check that QEMU has a target disk attached."),
                        COLOR_MUTED);
        return;
    }

    for (UINT32 i = 0; i < g_disks.count; i++)
    {
        int row_y = y + (int)i * 54;
        UINT32 bg = ((int)i == g_selected) ? 0xD9ECFFFF : COLOR_WHITE;
        xapi_DrawRect(g_window, x, row_y, x + w, row_y + 44, bg, true);
        xapi_DrawRect(g_window, x, row_y, x + w, row_y + 44, COLOR_PANEL_LINE, false);

        char line[160];
        char size_text[48];
        bytes_to_text(g_disks.disks[i].size_bytes, size_text, sizeof(size_text));
        if (g_language == XJ380_LANGUAGE_EN_US)
            snprintf(line, sizeof(line), "%s  |  %s  |  sector %u bytes",
                     g_disks.disks[i].name, size_text, g_disks.disks[i].sector_size);
        else
            snprintf(line, sizeof(line), "%s  |  %s  |  扇区 %u 字节",
                     g_disks.disks[i].name, size_text, g_disks.disks[i].sector_size);
        xapi_DrawSWText(g_window, x + 16, row_y + 13, line, COLOR_TEXT);
    }
}

static void draw_precheck_panel(int x, int y, int w, int h)
{
    xapi_DrawRect(g_window, x, y, x + w, y + h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, x, y, x + w, y + h, COLOR_PANEL_LINE, false);
    xapi_DrawSWText(g_window, x + 18, y + 18, inst_tr("安装前检查", "Pre-install checks"), COLOR_TEXT);
    run_precheck();

    int list_y = y + 52;
    for (UINT32 i = 0; i < g_precheck.item_count && i < 9; i++)
    {
        UINT32 color = COLOR_GREEN;
        char prefix[8] = "OK";
        if (g_precheck.items[i].status == XJ380_INSTALLER_CHECK_WARN)
        {
            color = COLOR_YELLOW;
            strcpy(prefix, "WARN");
        }
        else if (g_precheck.items[i].status == XJ380_INSTALLER_CHECK_ERROR)
        {
            color = COLOR_RED;
            strcpy(prefix, "ERR");
        }
        char title[96];
        snprintf(title, sizeof(title), "%s  %s", prefix, g_precheck.items[i].title);
        xapi_DrawSWText(g_window, x + 18, list_y + (int)i * 28, title, color);
        xapi_DrawSWText(g_window, x + 170, list_y + (int)i * 28, g_precheck.items[i].detail, COLOR_MUTED);
    }

    char space_line[160];
    char payload[48], required[48], target[48];
    bytes_to_text(g_precheck.payload_bytes, payload, sizeof(payload));
    bytes_to_text(g_precheck.required_bytes, required, sizeof(required));
    bytes_to_text(g_precheck.target_bytes, target, sizeof(target));
    if (g_language == XJ380_LANGUAGE_EN_US)
        snprintf(space_line, sizeof(space_line), "System files %s, required %s, target partition %s.",
                 payload, required, target);
    else
        snprintf(space_line, sizeof(space_line), "系统文件 %s，需要 %s，目标分区 %s。", payload, required, target);
    xapi_DrawSWText(g_window, x + 18, y + h - 44, space_line, COLOR_MUTED);
}

static void draw_home()
{
    draw_header(inst_tr("请选择目标硬盘和安装模式，安装前会先检查磁盘。",
                        "Choose a target disk and install mode. The disk will be checked first."));
    int margin = 30;
    int top = 118;
    int left_w = (int)g_width / 2 - 46;
    int right_x = margin + left_w + 28;
    int right_w = (int)g_width - right_x - margin;
    int panel_h = (int)g_height - top - 88;

    xapi_DrawRect(g_window, margin, top, margin + left_w, top + panel_h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, margin, top, margin + left_w, top + panel_h, COLOR_PANEL_LINE, false);
    xapi_DrawSWText(g_window, margin + 18, top + 18, inst_tr("目标硬盘", "Target disk"), COLOR_TEXT);
    draw_disk_rows(margin + 18, top + 80, left_w - 36);

    xapi_DrawRect(g_window, right_x, top, right_x + right_w, top + panel_h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, right_x, top, right_x + right_w, top + panel_h, COLOR_PANEL_LINE, false);
    draw_mode_cards(right_x + 18, top + 18, right_w - 36);
    draw_component_tree(right_x + 18, components_panel_y(), right_w - 36);

    UINT32 next_color = (g_selected >= 0 && !g_install_requested) ? COLOR_BLUE : COLOR_DISABLED;
    draw_button(margin, (int)g_height - 58, margin + 150, (int)g_height - 22,
                inst_tr("刷新硬盘", "Refresh"), COLOR_BLUE_DARK);
    draw_button((int)g_width - 200, (int)g_height - 58, (int)g_width - 30, (int)g_height - 22,
                inst_tr("下一步", "Next"), next_color);
}

static void draw_confirm()
{
    draw_header(inst_tr("请确认安装计划，确认后才会开始写入硬盘。",
                        "Confirm the install plan before writing to disk."));
    int margin = 42;
    int top = 124;
    int w = (int)g_width - margin * 2;
    int h = (int)g_height - top - 92;

    xapi_DrawRect(g_window, margin, top, margin + w, top + h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, margin, top, margin + w, top + h, COLOR_PANEL_LINE, false);
    xapi_DrawSWText(g_window, margin + 22, top + 18, inst_tr("安装确认", "Install confirmation"), COLOR_TEXT);

    run_precheck();
    if (g_selected >= 0 && g_selected < (int)g_disks.count)
    {
        char size_text[48];
        char line[220];
        bytes_to_text(g_disks.disks[g_selected].size_bytes, size_text, sizeof(size_text));
        if (g_language == XJ380_LANGUAGE_EN_US)
            snprintf(line, sizeof(line), "Target disk: %s, size %s, sector %u bytes.",
                     g_disks.disks[g_selected].name, size_text, g_disks.disks[g_selected].sector_size);
        else
            snprintf(line, sizeof(line), "目标硬盘：%s，容量 %s，扇区 %u 字节。",
                     g_disks.disks[g_selected].name, size_text, g_disks.disks[g_selected].sector_size);
        xapi_DrawSWText(g_window, margin + 22, top + 58, line, COLOR_TEXT);
        if (g_language == XJ380_LANGUAGE_EN_US)
            snprintf(line, sizeof(line), "Install mode: %s.", mode_name(g_mode));
        else
            snprintf(line, sizeof(line), "安装模式：%s。", mode_name(g_mode));
        xapi_DrawSWText(g_window, margin + 22, top + 88, line, COLOR_TEXT);
        xapi_DrawSWText(g_window, margin + 22, top + 118, (char *)mode_detail(g_mode), COLOR_MUTED);
        char component_text[220];
        components_to_text(component_text, sizeof(component_text));
        if (g_language == XJ380_LANGUAGE_EN_US)
            snprintf(line, sizeof(line), "Optional components: %s.", component_text);
        else
            snprintf(line, sizeof(line), "可选组件：%s。", component_text);
        xapi_DrawSWText(g_window, margin + 22, top + 148, line, COLOR_TEXT);
    }

    draw_precheck_panel(margin + 22, top + 186, w - 44, h - 210);

    draw_button(margin, (int)g_height - 58, margin + 120, (int)g_height - 22,
                inst_tr("返回", "Back"), COLOR_BLUE_DARK);
    UINT32 install_color = g_precheck.can_continue ? COLOR_RED : COLOR_DISABLED;
    draw_button((int)g_width - 230, (int)g_height - 58, (int)g_width - 30, (int)g_height - 22,
                inst_tr("确认并开始", "Confirm and start"), install_color);
}

static void draw_queue_panel(int x, int y, int w, int h)
{
    xapi_DrawSWText(g_window, x, y, inst_tr("文件安装队列", "File install queue"), COLOR_TEXT);
    char queue_head[64];
    if (g_progress.queue_total == 0)
        snprintf(queue_head, sizeof(queue_head), "0 / 0");
    else
        snprintf(queue_head, sizeof(queue_head), "%u / %u", g_progress.queue_index, g_progress.queue_total);
    xapi_DrawSWText(g_window, x + w - 96, y, queue_head, COLOR_MUTED);

    int list_y = y + 28;
    if (g_progress.queue_count == 0)
    {
        xapi_DrawSWText(g_window, x, list_y, inst_tr("正在整理文件列表。", "Preparing file list."), COLOR_MUTED);
        return;
    }

    int row_h = 20;
    int rows = (h - 28) / row_h;
    if (rows < 1) rows = 1;
    UINT32 visible_count = g_progress.queue_count;
    if (visible_count > (UINT32)rows) visible_count = (UINT32)rows;
    for (UINT32 i = 0; i < visible_count; i++)
    {
        char path_text[170];
        char line[210];
        shorten_path(g_progress.queue_items[i], path_text, sizeof(path_text));
        snprintf(line, sizeof(line), "%s%s",
                 i == 0 ? inst_tr("当前：", "Current: ") : inst_tr("后续：", "Next: "),
                 path_text);
        xapi_DrawSWText(g_window, x, list_y + (int)i * row_h, line, i == 0 ? COLOR_BLUE_DARK : COLOR_MUTED);
    }
}

static void draw_progress()
{
    draw_header(inst_tr("正在安装 XJ380，请不要关闭电源或移除目标硬盘。",
                        "Installing XJ380. Do not power off or remove the target disk."));
    int margin = 38;
    int top = 122;
    int w = (int)g_width - margin * 2;
    int h = (int)g_height - top - 42;

    xapi_DrawRect(g_window, margin, top, margin + w, top + h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, margin, top, margin + w, top + h, COLOR_PANEL_LINE, false);
    xapi_DrawSWText(g_window, margin + 22, top + 20, g_progress.stage, COLOR_BLUE_DARK);
    xapi_DrawSWText(g_window, margin + 22, top + 50, g_progress.detail, COLOR_MUTED);

    draw_progress_bar(margin + 22, top + 88, margin + w - 22, top + 118, g_progress.percent);
    char percent_text[64];
    if (g_language == XJ380_LANGUAGE_EN_US)
        snprintf(percent_text, sizeof(percent_text), "Total %u%%, current stage %u%%",
                 g_progress.percent, g_progress.stage_percent);
    else
        snprintf(percent_text, sizeof(percent_text), "总体进度 %u%%，当前阶段 %u%%",
                 g_progress.percent, g_progress.stage_percent);
    xapi_DrawSWText(g_window, margin + 22, top + 132, percent_text, COLOR_MUTED);

    char speed[48], eta[48], copied[48], total[48];
    speed_to_text(g_progress.bytes_per_second, speed, sizeof(speed));
    seconds_to_text(g_progress.eta_seconds, eta, sizeof(eta));
    bytes_to_text(g_progress.copied_bytes, copied, sizeof(copied));
    bytes_to_text(g_progress.total_bytes, total, sizeof(total));

    char stat[220];
    if (g_language == XJ380_LANGUAGE_EN_US)
        snprintf(stat, sizeof(stat), "Speed %s, remaining %s, copied %s / %s.", speed, eta, copied, total);
    else
        snprintf(stat, sizeof(stat), "速度 %s，剩余 %s，已复制 %s / %s。", speed, eta, copied, total);
    xapi_DrawSWText(g_window, margin + 22, top + 166, stat, COLOR_TEXT);
    if (g_language == XJ380_LANGUAGE_EN_US)
        snprintf(stat, sizeof(stat), "Small files %u / %u, large files %u / %u.",
                 g_progress.copied_small_file_count, g_progress.small_file_count,
                 g_progress.copied_large_file_count, g_progress.large_file_count);
    else
        snprintf(stat, sizeof(stat), "小文件 %u / %u，大文件 %u / %u。",
                 g_progress.copied_small_file_count, g_progress.small_file_count,
                 g_progress.copied_large_file_count, g_progress.large_file_count);
    xapi_DrawSWText(g_window, margin + 22, top + 196, stat, COLOR_TEXT);

    draw_queue_panel(margin + 22, top + 238, w - 44, h - 330);

    if (g_progress.state == XJ380_INSTALLER_DONE)
        xapi_DrawSWText(g_window, margin + 22, top + h - 54,
                        inst_tr("安装完成后请关闭虚拟机，移除 ISO，再从硬盘启动。",
                                "After installation, shut down, remove the ISO, and boot from disk."),
                        COLOR_GREEN);
    else if (g_progress.state == XJ380_INSTALLER_FAILED)
    {
        char error_line[128];
        if (g_language == XJ380_LANGUAGE_EN_US)
            snprintf(error_line, sizeof(error_line), "Installation failed, error: %lld", (long long)g_progress.result);
        else
            snprintf(error_line, sizeof(error_line), "安装失败，错误码: %lld", (long long)g_progress.result);
        xapi_DrawSWText(g_window, margin + 22, top + h - 54, error_line, COLOR_RED);
        xapi_DrawSWText(g_window, margin + 22, top + h - 28,
                        inst_tr("请打开日志页查看最近的安装器日志。",
                                "Open the log page to view recent installer logs."),
                        COLOR_MUTED);
    }
}

static void draw_rescue()
{
    draw_header(inst_tr("救援工具可以修复引导、检查磁盘、查看日志或打开终端。",
                        "Rescue tools can repair boot, check disks, view logs, or open a terminal."));
    int margin = 38;
    int top = 124;
    int left_w = 300;
    int right_x = margin + left_w + 24;
    int right_w = (int)g_width - right_x - margin;
    int h = (int)g_height - top - 42;

    xapi_DrawRect(g_window, margin, top, margin + left_w, top + h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, margin, top, margin + left_w, top + h, COLOR_PANEL_LINE, false);
    xapi_DrawSWText(g_window, margin + 18, top + 18, inst_tr("救援操作", "Rescue actions"), COLOR_TEXT);
    draw_button(margin + 18, top + 58, margin + 220, top + 94, inst_tr("重建 EFI 引导", "Rebuild EFI boot"), COLOR_BLUE);
    draw_button(margin + 18, top + 106, margin + 220, top + 142, inst_tr("检查分区", "Check partitions"), COLOR_BLUE);
    draw_button(margin + 18, top + 154, margin + 220, top + 190, inst_tr("查看硬盘", "View disk"), COLOR_BLUE);
    draw_button(margin + 18, top + 202, margin + 220, top + 238, inst_tr("打开终端", "Open terminal"), COLOR_BLUE);
    draw_button(margin + 18, top + 250, margin + 220, top + 286, inst_tr("查看日志", "View log"), COLOR_BLUE_DARK);

    xapi_DrawRect(g_window, right_x, top, right_x + right_w, top + h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, right_x, top, right_x + right_w, top + h, COLOR_PANEL_LINE, false);
    xapi_DrawSWText(g_window, right_x + 18, top + 18, inst_tr("救援结果", "Rescue result"), COLOR_TEXT);
    for (UINT32 i = 0; i < g_rescue.item_count && i < 14; i++)
    {
        UINT32 color = g_rescue.items[i].status == XJ380_INSTALLER_CHECK_ERROR ? COLOR_RED :
                       (g_rescue.items[i].status == XJ380_INSTALLER_CHECK_WARN ? COLOR_YELLOW : COLOR_GREEN);
        int y = top + 58 + (int)i * 32;
        xapi_DrawSWText(g_window, right_x + 18, y, g_rescue.items[i].title, color);
        xapi_DrawSWText(g_window, right_x + 180, y, g_rescue.items[i].detail, COLOR_MUTED);
    }
}

static void draw_log()
{
    draw_header(inst_tr("最近的安装器日志会显示在这里。", "Recent installer logs are shown here."));
    xapi_InstallerLog(&g_log);
    int margin = 38;
    int top = 124;
    int w = (int)g_width - margin * 2;
    int h = (int)g_height - top - 42;
    xapi_DrawRect(g_window, margin, top, margin + w, top + h, COLOR_PANEL, true);
    xapi_DrawRect(g_window, margin, top, margin + w, top + h, COLOR_PANEL_LINE, false);
    xapi_DrawSWText(g_window, margin + 18, top + 18, inst_tr("安装日志", "Install log"), COLOR_TEXT);
    if (g_log.count == 0)
    {
        xapi_DrawSWText(g_window, margin + 18, top + 58, inst_tr("暂无日志。", "No logs yet."), COLOR_MUTED);
        return;
    }
    for (UINT32 i = 0; i < g_log.count && i < XJ380_INSTALLER_LOG_LINES; i++)
        xapi_DrawSWText(g_window, margin + 18, top + 58 + (int)i * 22, g_log.lines[i], COLOR_MUTED);
}

static void render()
{
    if (g_page == PAGE_CONFIRM) draw_confirm();
    else if (g_page == PAGE_PROGRESS) draw_progress();
    else if (g_page == PAGE_RESCUE) draw_rescue();
    else if (g_page == PAGE_LOG) draw_log();
    else draw_home();
    xapi_RefreshWindow(g_window);
    g_need_redraw = false;
}

static void start_install()
{
    if (g_selected < 0 || g_selected >= (int)g_disks.count || g_install_requested) return;
    run_precheck();
    if (!g_precheck.can_continue)
    {
        set_local_failure(-1, inst_tr("安装前检查失败", "Pre-install check failed"),
                          inst_tr("目标硬盘未通过安装前检查。", "The target disk did not pass pre-install checks."));
        return;
    }
    xj380_installer_start_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.disk_id = g_disks.disks[g_selected].id;
    opts.mode = g_mode;
    opts.language = (UINT32)xj380_normalize_language(g_language);
    opts.components = normalize_components(g_components);
    INT64 ret = (INT64)xapi_InstallerStartOptions(&opts);
    if (ret < 0)
    {
        set_local_failure(ret, inst_tr("无法开始安装", "Unable to start installation"),
                          inst_tr("内核拒绝启动安装任务，请确认当前是 ISO 安装模式。",
                                  "The kernel refused to start the install task. Check that this is ISO install mode."));
        return;
    }
    g_install_requested = true;
    g_page = PAGE_PROGRESS;
    g_need_redraw = true;
}

static void run_rescue(UINT64 action)
{
    memset(&g_rescue, 0, sizeof(g_rescue));
    if (action == XJ380_INSTALLER_RESCUE_VIEW_LOG)
    {
        g_page = PAGE_LOG;
        g_need_redraw = true;
        return;
    }
    if (action == XJ380_INSTALLER_RESCUE_OPEN_TERM)
    {
        xapi_InstallerRescue(action, 0, &g_rescue);
    }
    else if (g_selected < 0 || g_selected >= (int)g_disks.count)
    {
        g_rescue.result = -1;
        g_rescue.item_count = 1;
        g_rescue.items[0].status = XJ380_INSTALLER_CHECK_ERROR;
        strcpy(g_rescue.items[0].title, inst_tr("未选择硬盘", "No disk selected"));
        strcpy(g_rescue.items[0].detail,
               inst_tr("请先返回主页选择目标硬盘。", "Return to Home and choose a target disk first."));
    }
    else
    {
        xapi_InstallerRescue(action, g_disks.disks[g_selected].id, &g_rescue);
    }
    if (action == XJ380_INSTALLER_RESCUE_OPEN_TERM && g_rescue.result >= 0 && g_window != 0)
    {
        xapi_CloseWindow(g_window);
        g_window = 0;
        g_window_closed_for_terminal = true;
        return;
    }
    g_page = PAGE_RESCUE;
    g_need_redraw = true;
}

static void handle_home_click(UINT64 x, UINT64 y)
{
    int margin = 30;
    int top = 118;
    int left_w = (int)g_width / 2 - 46;
    int right_x = margin + left_w + 28;
    int row = disk_row_at(y);
    if (row >= 0 && x >= (UINT64)(margin + 18) && x <= (UINT64)(margin + left_w - 18))
    {
        g_selected = row;
        g_need_redraw = true;
        return;
    }
    for (int i = 0; i < 4; i++)
    {
        int row_y = top + 18 + 30 + i * 58;
        if (in_rect(x, y, right_x + 18, row_y, (int)g_width - margin - 18, row_y + 46))
        {
            g_mode = (UINT32)i;
            g_need_redraw = true;
            return;
        }
    }
    int component_row = component_row_at(y);
    if (component_row >= 0 && in_rect(x, y, right_x + 18, components_panel_y() + 28,
                                      (int)g_width - margin - 18, components_panel_y() + 150))
    {
        toggle_component_row(component_row);
        g_need_redraw = true;
        return;
    }
    if (in_rect(x, y, margin, (int)g_height - 58, margin + 150, (int)g_height - 22))
    {
        if (!g_install_requested) refresh_disks();
    }
    else if (in_rect(x, y, (int)g_width - 200, (int)g_height - 58, (int)g_width - 30, (int)g_height - 22))
    {
        if (g_selected >= 0 && !g_install_requested)
        {
            run_precheck();
            g_page = PAGE_CONFIRM;
            g_need_redraw = true;
        }
    }
}

static void message_handler(UINT64 type, UINT64 hData, UINT64 lData)
{
    switch (type)
    {
    case MSG_LBUTTON:
    {
        if (handle_language_click(hData, lData)) break;

        int right = (int)g_width - 360;
        if (in_rect(hData, lData, right, 28, right + 96, 64))
        {
            if (!g_install_requested) g_page = PAGE_HOME;
            g_need_redraw = true;
            break;
        }
        if (in_rect(hData, lData, right + 108, 28, right + 204, 64))
        {
            g_page = PAGE_RESCUE;
            g_need_redraw = true;
            break;
        }
        if (in_rect(hData, lData, right + 216, 28, right + 312, 64))
        {
            g_page = PAGE_LOG;
            g_need_redraw = true;
            break;
        }

        if (g_page == PAGE_HOME) handle_home_click(hData, lData);
        else if (g_page == PAGE_CONFIRM)
        {
            if (in_rect(hData, lData, 42, (int)g_height - 58, 162, (int)g_height - 22))
            {
                g_page = PAGE_HOME;
                g_need_redraw = true;
            }
            else if (in_rect(hData, lData, (int)g_width - 230, (int)g_height - 58, (int)g_width - 30, (int)g_height - 22))
            {
                if (g_precheck.can_continue) start_install();
            }
        }
        else if (g_page == PAGE_RESCUE)
        {
            int margin = 38;
            int top = 124;
            if (in_rect(hData, lData, margin + 18, top + 58, margin + 220, top + 94))
                run_rescue(XJ380_INSTALLER_RESCUE_REBUILD_BOOT);
            else if (in_rect(hData, lData, margin + 18, top + 106, margin + 220, top + 142))
                run_rescue(XJ380_INSTALLER_RESCUE_CHECK_DISK);
            else if (in_rect(hData, lData, margin + 18, top + 154, margin + 220, top + 190))
                run_rescue(XJ380_INSTALLER_RESCUE_VIEW_DISK);
            else if (in_rect(hData, lData, margin + 18, top + 202, margin + 220, top + 238))
                run_rescue(XJ380_INSTALLER_RESCUE_OPEN_TERM);
            else if (in_rect(hData, lData, margin + 18, top + 250, margin + 220, top + 286))
                run_rescue(XJ380_INSTALLER_RESCUE_VIEW_LOG);
        }
        break;
    }
    case MSG_RESIZE:
        g_width = hData;
        g_height = lData;
        g_need_redraw = true;
        break;
    default:
        break;
    }
}

static int installer_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    memset(&g_progress, 0, sizeof(g_progress));
    g_progress.state = XJ380_INSTALLER_IDLE;
    strcpy(g_progress.stage, inst_tr("等待安装", "Waiting to install"));
    strcpy(g_progress.detail, inst_tr("请选择目标硬盘和安装模式。", "Choose a target disk and install mode."));

    XWINDOW window;
    window.width = 0;
    window.height = 0;
    window.title = inst_tr("XJ380 安装程序", "XJ380 Installer");
    window.sets = XWIN_FULL_SCR;
    xapi_CreateWindow(&g_window, &window);
    xapi_GetWindowSize(g_window, &g_width, &g_height);
    SetMsgPrcor(g_window, message_handler);
    refresh_disks();

    while (true)
    {
        if (g_window_closed_for_terminal)
        {
            xapi_Sleep(1000);
            continue;
        }
        if (g_install_requested)
        {
            xj380_installer_progress latest;
            memset(&latest, 0, sizeof(latest));
            if ((INT64)xapi_InstallerProgress(&latest) >= 0 && !progress_equal(&latest, &g_progress))
            {
                memcpy(&g_progress, &latest, sizeof(g_progress));
                g_need_redraw = true;
            }
        }
        if (g_page == PAGE_LOG) g_need_redraw = true;
        if (g_need_redraw) render();
        xapi_Sleep(100);
    }
    return 0;
}

extern "C" int installer_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int installer_main_cpp(int argc, char *argv[], char *envp[])
{
    return installer_main_impl(argc, argv, envp);
}

extern "C" int main(int argc, char *argv[], char *envp[])
{
    return installer_main_impl(argc, argv, envp);
}
