// 版权所有©XINGJI Studios 2017-2026 保留所有权利。
// XJ380UEFI引导程序（\EFI\BOOT\BOOTX64.efi）
#include "../kernel/build_settings.h"
#include "bootlib.h"
#include <acpi.h>
#include <boot.h>
#include <efi.h>
#include <elf.h>
#include <fbc.h>
#include <msr.h>
#include "memory.h"

#define NULL 0
struct EFI_SYSTEM_TABLE                *ST;   // 系统表
struct EFI_BOOT_SERVICES               *BS;   // 启动信息
struct EFI_GRAPHICS_OUTPUT_PROTOCOL    *GOP;  // GOP
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SFSP; // 文件
struct EFI_LOADED_IMAGE_PROTOCOL       *LIP;  // 文件
struct EFI_SIMPLE_POINTER_PROTOCOL     *SPP;  // 鼠标
struct EFI_BATTERY_CHARGING_PROTOCOL   *BAT;  // 电池
EFI_HANDLE                              IM;

#define EFI_SCAN_UP     0x0001
#define EFI_SCAN_DOWN   0x0002
#define EFI_SCAN_DELETE 0x0008
#define EFI_SCAN_ESC    0x0017

#define BOOT_MENU_WAIT_US 300000
#define BOOT_MENU_POLL_US 10000

#define BOOT_MENU_OPTION_COUNT 9
#define BOOT_COLOR_NORMAL 0x0f
#define BOOT_COLOR_MUTED 0x07
#define BOOT_COLOR_PANEL 0x1f
#define BOOT_COLOR_SELECTED 0x1f
#define BOOT_COLOR_TITLE 0x0b
#define BOOT_COLOR_WARNING 0x0e

typedef struct
{
    const char *title;
    const char *description;
    UINT64      flags;
    int         reboot;
} BOOT_MENU_OPTION;

static UINT64 BootMenuFlags;
static char   BootStatusLines[8][80];
static int    BootStatusCount;
static int    BootProgressStep;
static int    BootRenderedProgressStep;
static int    BootProgressScreenDrawn;

static const BOOT_MENU_OPTION BootMenuOptions[BOOT_MENU_OPTION_COUNT] = {
    {"Safe Mode",
     "Start with loadable kernel modules disabled and conservative storage I/O.",
     BOOT_FLAG_SAFE_MODE | BOOT_FLAG_DISABLE_KMOD | BOOT_FLAG_SAFE_STORAGE_IO,
     0},
    {"Safe Mode with Networking",
     "Start in Safe Mode while keeping the networking stack available.",
     BOOT_FLAG_SAFE_MODE | BOOT_FLAG_DISABLE_KMOD | BOOT_FLAG_SAFE_STORAGE_IO,
     0},
    {"Safe Mode with Command Prompt",
     "Start in Safe Mode and keep the system on the basic startup path.",
     BOOT_FLAG_SAFE_MODE | BOOT_FLAG_DISABLE_KMOD | BOOT_FLAG_SAFE_STORAGE_IO,
     0},
    {"Enable Boot Logging",
     "Start normally and write verbose boot diagnostics to the serial log.",
     BOOT_FLAG_VERBOSE,
     0},
    {"Show Boot Progress",
     "Display bootloader progress and startup stage details.",
     BOOT_FLAG_SHOW_PROGRESS,
     0},
    {"Enable low-resolution video",
     "Start with a basic 800x600 video mode.",
     BOOT_FLAG_BASE_VIDEO,
     0},
    {"Last Known Good Configuration",
     "Start with the last stable configuration profile.",
     BOOT_FLAG_LAST_KNOWN_GOOD | BOOT_FLAG_SAFE_STORAGE_IO,
     0},
    {"Disable loadable kernel modules",
     "Start without loading files from /system as kernel modules.",
     BOOT_FLAG_DISABLE_KMOD,
     0},
    {"Start XJ380 Normally",
     "Start XJ380 with the standard kernel startup path.",
     0,
     0},
};

void   write_serial_string(char *str);
char  *strcpy(char *dest, const char *src);
UINT64 strcmp(char *from_str, char *cmp_str);
UINT64 part_strcmp(char *from_str, char *cmp_str, UINT64 size);

/**
 * @brief 将无符号64位整数转换为16位字符串
 *
 * @param buf 目标缓冲区，用于存储转换后的字符串
 * @param len 目标缓冲区的长度
 * @param n 要转换的无符号64位整数
 * @return 返回指向转换后字符串的指针，如果缓冲区长度不足，则返回空指针
 */
static inline __attribute__((always_inline)) UINT16 *u64tostrb16(UINT16 *buf, size_t len, UINT64 n)
{
    UINT16 *s = buf + len;
    *--s      = '\0';
    if (n == 0) return (*--s = '0', s);
    for (; n; n >>= 4)
        *--s = L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"[n & 15];
    return s;
}

// 图形输出协议 GUID
EFI_GUID gop_guid = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};
// 文件加载协议 GUID
EFI_GUID lip_guid = {
    0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
// 简单文件系统协议 GUID
EFI_GUID sfsp_guid = {
    0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
// 简单指针输入协议 GUID
EFI_GUID spp_guid = {
    0x31878c87, 0xb75, 0x11d5, {0x9a, 0x4f, 0x0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};
// 电池协议 GUID
EFI_GUID battery_guid = {
    0x840cb643, 0x8198, 0x428a, {0xa8, 0xb3, 0xa0, 0x72, 0xce, 0x57, 0xcd, 0xb9}
};
// ACPI 1.x GUID
EFI_GUID acpi_table_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};
// ACPI 2.0或更新 GUID
EFI_GUID efi_acpi_table_guid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};
EFI_GUID efi_file_info_guid = {
    0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

/**
 * @brief 输出一个16位的字符到控制台
 *
 * @param c 要输出的16位字符
 */
void putc(unsigned short c)
{
    __WCHAR_TYPE__ str[2] = L" ";
    str[0]                = c;
    ST->ConOut->OutputString(ST->ConOut, str);
}

void puts(unsigned short *s)
{
    ST->ConOut->OutputString(ST->ConOut, s);
}

static void puts_ascii(const char *s)
{
    CHAR16 buffer[128];
    UINTN  pos = 0;

    while (*s)
    {
        buffer[pos++] = (CHAR16)*s;
        s++;
        if (pos == sizeof(buffer) / sizeof(buffer[0]) - 1)
        {
            buffer[pos] = L'\0';
            puts(buffer);
            pos = 0;
        }
    }

    if (pos > 0)
    {
        buffer[pos] = L'\0';
        puts(buffer);
    }
}

static void puts_repeat(char ch, int count)
{
    CHAR16 buffer[128];
    while (count > 0)
    {
        int chunk = count;
        if (chunk > (int)(sizeof(buffer) / sizeof(buffer[0]) - 1))
            chunk = (int)(sizeof(buffer) / sizeof(buffer[0]) - 1);
        for (int i = 0; i < chunk; i++)
            buffer[i] = (CHAR16)ch;
        buffer[chunk] = L'\0';
        puts(buffer);
        count -= chunk;
    }
}

static void boot_console_attr(UINTN attr)
{
    ST->ConOut->SetAttribute(ST->ConOut, attr);
}

static void boot_clear(UINTN attr)
{
    boot_console_attr(attr);
    ST->ConOut->ClearScreen(ST->ConOut);
}

static void boot_move(UINTN col, UINTN row)
{
    ST->ConOut->SetCursorPosition(ST->ConOut, col, row);
}

static void boot_hide_cursor(void)
{
    ST->ConOut->EnableCursor(ST->ConOut, 0);
}

static void boot_hr(void)
{
    boot_console_attr(BOOT_COLOR_PANEL);
    puts_ascii("  ");
    puts_repeat(' ', 76);
    puts(L"\r\n");
    boot_console_attr(BOOT_COLOR_NORMAL);
}

static void boot_header(const char *title, const char *subtitle)
{
    boot_clear(BOOT_COLOR_NORMAL);
    boot_hide_cursor();
    boot_console_attr(BOOT_COLOR_PANEL);
    puts(L"  XJ380 Boot Manager                                                        \r\n");
    puts(L"                                                                            \r\n");
    boot_console_attr(BOOT_COLOR_TITLE);
    puts_ascii("  ");
    puts_ascii(title);
    puts(L"\r\n");
    boot_console_attr(BOOT_COLOR_MUTED);
    if (subtitle != NULL)
    {
        puts_ascii("  ");
        puts_ascii(subtitle);
        puts(L"\r\n");
    }
    puts(L"\r\n");
    boot_hr();
}

static void puts_ascii_padded(const char *s, int width)
{
    int len = 0;
    if (s != NULL)
    {
        while (s[len] != '\0' && len < width)
            len++;
        puts_ascii(s);
    }
    if (width > len) puts_repeat(' ', width - len);
}

static void puts_dec(UINT64 value)
{
    char  buf[24];
    UINTN pos = 0;
    UINTN len = 0;

    if (value == 0)
    {
        puts(L"0");
        return;
    }

    while (value > 0 && len < sizeof(buf))
    {
        buf[len++] = (char)('0' + (value % 10));
        value /= 10;
    }

    CHAR16 out[24];
    while (pos < len)
    {
        out[pos] = (CHAR16)buf[len - 1 - pos];
        pos++;
    }
    out[pos] = L'\0';
    puts(out);
}

static void boot_progress_bar(int step, int total)
{
    int width = 42;
    int filled;
    char bar[48];
    if (total <= 0) total = 1;
    if (step < 0) step = 0;
    if (step > total) step = total;
    filled = width * step / total;

    boot_console_attr(BOOT_COLOR_NORMAL);
    puts_ascii("  [");
    boot_console_attr(BOOT_COLOR_TITLE);
    for (int i = 0; i < width; i++)
        bar[i] = i < filled ? '=' : ' ';
    bar[width] = '\0';
    puts_ascii(bar);
    boot_console_attr(BOOT_COLOR_NORMAL);
    puts_ascii("] ");
    puts_dec((UINT64)(step * 100 / total));
    puts_ascii("%");
    puts(L"\r\n");
}

static void boot_text_status_screen(const char *status)
{
    if (!BootProgressScreenDrawn)
    {
        boot_header("Starting XJ380", "Firmware is preparing the operating system.");
        boot_console_attr(BOOT_COLOR_NORMAL);
        puts(L"  Current stage: \r\n\r\n");
        puts(L"\r\n");
        boot_console_attr(BOOT_COLOR_MUTED);
        puts(L"  Boot log\r\n");
        BootProgressScreenDrawn = 1;
    }

    boot_move(17, 7);
    boot_console_attr(BOOT_COLOR_TITLE);
    puts_ascii_padded(status != NULL ? status : "working", 58);

    boot_move(0, 9);
    boot_progress_bar(BootProgressStep, 10);

    boot_console_attr(BOOT_COLOR_NORMAL);
    for (int i = 0; i < BootStatusCount; i++)
    {
        boot_move(0, 12 + i);
        puts_ascii("    ");
        puts_ascii_padded(BootStatusLines[i], 70);
    }
}

static void boot_prompt_screen(void)
{
    boot_header("XJ380", "Press DELETE for Advanced Boot Options.");
    boot_console_attr(BOOT_COLOR_NORMAL);
    puts(L"  The system will start automatically.\r\n");
    puts(L"\r\n");
    boot_console_attr(BOOT_COLOR_MUTED);
    puts(L"  DELETE  Advanced options\r\n");
    puts(L"  ESC     Continue startup\r\n");
}

static void boot_status(const char *status)
{
    if (status != NULL)
    {
        if (BootProgressStep < 10) BootProgressStep++;
        if (BootStatusCount < 8)
        {
            UINTN i = 0;
            while (status[i] && i < sizeof(BootStatusLines[0]) - 1)
            {
                BootStatusLines[BootStatusCount][i] = status[i];
                i++;
            }
            BootStatusLines[BootStatusCount][i] = '\0';
            BootStatusCount++;
        }
        else
        {
            for (int line = 1; line < 8; line++)
                xmemcpy(BootStatusLines[line - 1], BootStatusLines[line], sizeof(BootStatusLines[0]));
            UINTN i = 0;
            while (status[i] && i < sizeof(BootStatusLines[0]) - 1)
            {
                BootStatusLines[7][i] = status[i];
                i++;
            }
            BootStatusLines[7][i] = '\0';
        }
    }
    if ((BootMenuFlags & BOOT_FLAG_SHOW_PROGRESS) != 0)
    {
        int should_render = BootRenderedProgressStep == 0 || BootProgressStep >= 10 ||
                            BootProgressStep >= BootRenderedProgressStep + 3;
        if (should_render)
        {
            BootRenderedProgressStep = BootProgressStep;
            boot_text_status_screen(status);
        }
    }
}

static void boot_menu_init_defaults(void)
{
    BootMenuFlags = 0;
    BootStatusCount = 0;
    BootProgressStep = 0;
    BootRenderedProgressStep = 0;
    BootProgressScreenDrawn = 0;
}

static int read_boot_key(struct EFI_INPUT_KEY *key)
{
    xmemset(key, 0, sizeof(*key));
    return ST->ConIn->ReadKeyStroke(ST->ConIn, key) == EFI_SUCCESS;
}

static void drain_boot_keys(void)
{
    struct EFI_INPUT_KEY key;
    while (read_boot_key(&key))
        ;
}

static int wait_for_boot_menu_key(void)
{
    UINTN waited = 0;
    struct EFI_INPUT_KEY key;

    drain_boot_keys();
    boot_prompt_screen();

    while (waited < BOOT_MENU_WAIT_US)
    {
        if (read_boot_key(&key))
        {
            if (key.ScanCode == EFI_SCAN_DELETE) return 1;
            if (key.ScanCode == EFI_SCAN_ESC || key.UnicodeChar == L'\r') return 0;
        }

        BS->Stall(BOOT_MENU_POLL_US);
        waited += BOOT_MENU_POLL_US;
    }

    return 0;
}

static void draw_boot_menu_item_at(int index, int selected, int row)
{
    boot_move(0, (UINTN)row);
    boot_console_attr(selected ? BOOT_COLOR_SELECTED : BOOT_COLOR_NORMAL);
    puts_ascii("  ");
    if (selected)
    {
        char number[2] = {(char)('1' + index), '\0'};
        puts_ascii("> ");
        puts_ascii(number);
        puts_ascii(". ");
        puts_ascii(BootMenuOptions[index].title);
        int pad = 44 - (int)strlen(BootMenuOptions[index].title);
        if (pad < 1) pad = 1;
        puts_repeat(' ', pad);
    }
    else
    {
        char number[2] = {(char)('1' + index), '\0'};
        puts_ascii("  ");
        puts_ascii(number);
        puts_ascii(". ");
        puts_ascii(BootMenuOptions[index].title);
    }
    boot_console_attr(BOOT_COLOR_NORMAL);
    puts_repeat(' ', 28);
}

static void draw_boot_menu_description(int selected)
{
    boot_move(0, 18);
    boot_console_attr(BOOT_COLOR_PANEL);
    puts(L"  Selected option                                                           ");
    boot_move(0, 19);
    boot_console_attr(BOOT_COLOR_NORMAL);
    puts_ascii("  ");
    puts_ascii_padded(BootMenuOptions[selected].title, 72);
    boot_move(0, 20);
    boot_console_attr(BOOT_COLOR_MUTED);
    puts_ascii("  ");
    puts_ascii_padded(BootMenuOptions[selected].description, 72);
    boot_move(0, 22);
    boot_console_attr(BOOT_COLOR_WARNING);
    puts(L"  ENTER=Start selected    ESC=Start normally    DELETE=Stay in this menu   ");
    boot_console_attr(BOOT_COLOR_NORMAL);
}

static void draw_boot_menu_full(int selected)
{
    boot_header("Advanced Boot Options", "Choose how XJ380 should start.");
    boot_console_attr(BOOT_COLOR_MUTED);
    puts(L"  Use UP/DOWN to select. Press ENTER to boot. Number keys choose directly.\r\n");
    puts(L"\r\n");

    for (int i = 0; i < BOOT_MENU_OPTION_COUNT; i++)
        draw_boot_menu_item_at(i, selected == i, 7 + i);

    draw_boot_menu_description(selected);
}

static void update_boot_menu_selection(int previous, int selected)
{
    if (previous >= 0 && previous < BOOT_MENU_OPTION_COUNT)
        draw_boot_menu_item_at(previous, 0, 7 + previous);
    draw_boot_menu_item_at(selected, 1, 7 + selected);
    draw_boot_menu_description(selected);
}

static int apply_boot_menu_action(int selected)
{
    if (selected < 0 || selected >= BOOT_MENU_OPTION_COUNT) return 0;
    if (BootMenuOptions[selected].reboot)
    {
        ST->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
        return 0;
    }

    BootMenuFlags = BootMenuOptions[selected].flags;
    return 1;
}

static void boot_menu(void)
{
    int selected = 0;
    int previous;
    struct EFI_INPUT_KEY key;

    drain_boot_keys();
    draw_boot_menu_full(selected);
    while (1)
    {
        while (!read_boot_key(&key))
            BS->Stall(BOOT_MENU_POLL_US);

        if (key.ScanCode == EFI_SCAN_UP)
        {
            previous = selected;
            selected = selected == 0 ? BOOT_MENU_OPTION_COUNT - 1 : selected - 1;
            update_boot_menu_selection(previous, selected);
        }
        else if (key.ScanCode == EFI_SCAN_DOWN)
        {
            previous = selected;
            selected = selected == BOOT_MENU_OPTION_COUNT - 1 ? 0 : selected + 1;
            update_boot_menu_selection(previous, selected);
        }
        else if (key.ScanCode == EFI_SCAN_ESC)
        {
            BootMenuFlags = 0;
            return;
        }
        else if (key.UnicodeChar == L'\r')
        {
            if (apply_boot_menu_action(selected)) return;
        }
        else if (key.UnicodeChar >= L'1' && key.UnicodeChar <= L'9')
        {
            previous = selected;
            selected = (int)(key.UnicodeChar - L'1');
            update_boot_menu_selection(previous, selected);
            if (apply_boot_menu_action(selected)) return;
        }
    }
}

/**
 * @brief 将无符号64位整数转换为16位字符串并输出到控制台
 *
 * @param hex 要转换的无符号64位整数
 */
void puth(unsigned long long hex)
{
    unsigned short  buf[20];
    unsigned short *p = buf;
    unsigned short  ch;
    int             i, flag = 0;

    *p++ = L'0';
    *p++ = L'x'; // 先存一个0x

    if (hex == 0)
        *p++ = L'0'; // 如果是0，直接0x0趋势
    else
    {
        for (i = 28; i >= 0; i -= 4)
        {                          // 每次4位
            ch = (hex >> i) & 0xF; // 0~9, A~F
            // 28（冗余）
            if (flag || ch > 0)
            {                 // 跳过前导0
                flag  = 1;    // 没有前导0就把flag设为1，这样后面再有0也不会忽略
                ch   += L'0'; // 0~9 => '0'~'9'
                if (ch > L'9')
                {
                    ch += 7; // 'A' - '9' = 7
                }
                *p++ = ch; // 写入
            }
        }
    }
    *p = L'\0'; // 结束符
    puts(buf);  // 打印
}

void *malloc(size_t buf_size)
{
    void              *res;
    unsigned long long status;

    status = BS->AllocatePool(EfiLoaderData, buf_size,
                              &res); // 分配内存
    if (status != EFI_SUCCESS) return NULL;

    return res;
}

void free(void *buf)
{
    BS->FreePool(buf);
}

static EFI_STATUS open_boot_file(struct EFI_FILE_PROTOCOL *root, CHAR16 *path, struct EFI_FILE_PROTOCOL **file)
{
    return root->Open(root, file, path, EFI_FILE_MODE_READ, 0);
}

static EFI_STATUS boot_file_exists(struct EFI_FILE_PROTOCOL *root, CHAR16 *path)
{
    if (root == NULL || path == NULL) return EFI_INVALID_PARAMETER;

    struct EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status = open_boot_file(root, path, &file);
    if (EFI_ERROR(status)) return status;

    file->Close(file);
    return EFI_SUCCESS;
}

static int boot_volume_has_system_markers(struct EFI_FILE_PROTOCOL *root)
{
    return !EFI_ERROR(boot_file_exists(root, L"\\EFI\\BOOT\\BOOTX64.EFI")) &&
           !EFI_ERROR(boot_file_exists(root, L"\\system\\kernel.krl"));
}

static EFI_STATUS open_system_volume(struct EFI_FILE_PROTOCOL **out_root)
{
    if (out_root == NULL) return EFI_INVALID_PARAMETER;
    *out_root = NULL;

    EFI_HANDLE *handles = NULL;
    UINTN       handle_count = 0;
    EFI_STATUS  status = BS->LocateHandleBuffer(ByProtocol, &sfsp_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status)) return status;

    for (UINTN i = 0; i < handle_count; i++)
    {
        struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        status = BS->OpenProtocol(handles[i], &sfsp_guid, (void **)&fs, IM, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(status) || fs == NULL) continue;

        struct EFI_FILE_PROTOCOL *candidate = NULL;
        status = fs->OpenVolume(fs, &candidate);
        if (EFI_ERROR(status) || candidate == NULL) continue;

        if (boot_volume_has_system_markers(candidate))
        {
            *out_root = candidate;
            BS->FreePool(handles);
            return EFI_SUCCESS;
        }

        candidate->Close(candidate);
    }

    BS->FreePool(handles);
    return EFI_NOT_FOUND;
}

static EFI_STATUS boot_file_size(struct EFI_FILE_PROTOCOL *file, UINTN *out_size)
{
    if (file == NULL || out_size == NULL) return EFI_INVALID_PARAMETER;

    UINTN info_size = sizeof(struct EFI_FILE_INFO) + 256 * sizeof(CHAR16);
    struct EFI_FILE_INFO *info = (struct EFI_FILE_INFO *)malloc(info_size);
    if (info == NULL) return EFI_OUT_OF_RESOURCES;

    EFI_STATUS status = file->GetInfo(file, &efi_file_info_guid, &info_size, info);
    if (status == EFI_BUFFER_TOO_SMALL)
    {
        free(info);
        info = (struct EFI_FILE_INFO *)malloc(info_size);
        if (info == NULL) return EFI_OUT_OF_RESOURCES;
        status = file->GetInfo(file, &efi_file_info_guid, &info_size, info);
    }

    if (!EFI_ERROR(status))
        *out_size = (UINTN)info->FileSize;

    free(info);
    return status;
}

static EFI_STATUS read_boot_file(struct EFI_FILE_PROTOCOL *root, CHAR16 *path, void **out_buffer, UINTN *out_size)
{
    if (root == NULL || path == NULL || out_buffer == NULL || out_size == NULL) return EFI_INVALID_PARAMETER;
    *out_buffer = NULL;
    *out_size = 0;

    struct EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status = open_boot_file(root, path, &file);
    if (EFI_ERROR(status)) return status;

    UINTN size = 0;
    status = boot_file_size(file, &size);
    if (EFI_ERROR(status) || size == 0)
    {
        file->Close(file);
        return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
    }

    void *buffer = malloc(size);
    if (buffer == NULL)
    {
        file->Close(file);
        return EFI_OUT_OF_RESOURCES;
    }
    xmemset(buffer, 0, size);

    UINTN read_size = size;
    status = file->Read(file, &read_size, buffer);
    file->Close(file);
    if (EFI_ERROR(status) || read_size != size)
    {
        free(buffer);
        return EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;
    }

    *out_buffer = buffer;
    *out_size = size;
    return EFI_SUCCESS;
}

EFI_STATUS mallocAt(EFI_PHYSICAL_ADDRESS addr, UINTN size)
{
    EFI_STATUS           status;
    EFI_PHYSICAL_ADDRESS allocated_addr = addr;
    status = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, (size + 0xfff) / 0x1000, &allocated_addr);
    return status;
}

void freeAt(EFI_PHYSICAL_ADDRESS addr, UINTN size)
{
    BS->FreePages(addr, (size + 0xfff) / 0x1000);
}

void efi_init(EFI_HANDLE ImageHandle, struct EFI_SYSTEM_TABLE *SystemTable)
{
    ST = SystemTable;
    BS = SystemTable->BootServices;
    IM = ImageHandle;

    // 初始化
    BS->SetWatchdogTimer(0, 0, 0, NULL); // 别删，不然UEFI会自动重启
    BS->LocateProtocol(&gop_guid, NULL, (void **)&GOP);
    BS->LocateProtocol(&spp_guid, NULL, (void **)&SPP);
    BS->LocateProtocol(&battery_guid, NULL, (void **)&BAT);

    BS->OpenProtocol(ImageHandle, &lip_guid, (void **)&LIP, ImageHandle, NULL,
                     EFI_OPEN_PROTOCOL_GET_PROTOCOL); // 获取LIP
    BS->OpenProtocol(LIP->DeviceHandle, &sfsp_guid, (void **)&SFSP, ImageHandle, NULL,
                     EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL); // 获取SFSP
}

EFI_STATUS GetMMP(MEMORY_MAP *MemoryMap)
{
    EFI_STATUS GetMemoryMapStatus = EFI_SUCCESS;

    // 获取内存表
    MemoryMap->Buffer = malloc(MemoryMap->MapSize);
    xmemset(MemoryMap->Buffer,0,MemoryMap->MapSize);

    // 获取内存表
    while (BS->GetMemoryMap(&MemoryMap->MapSize, (EFI_MEMORY_DESCRIPTOR *)MemoryMap->Buffer, &MemoryMap->MapKey,
                            &MemoryMap->DescriptorSize, &MemoryMap->DescriptorVersion) == EFI_BUFFER_TOO_SMALL)
    {
        if (MemoryMap->Buffer)
        {
            free(MemoryMap->Buffer);
            MemoryMap->Buffer = NULL;
        }

        // 重新分配更大的缓冲区
        MemoryMap->Buffer = malloc(MemoryMap->MapSize);
        xmemset(MemoryMap->Buffer,0,MemoryMap->MapSize);
    }

    // 检查是否成功分配了内存
    if (!MemoryMap->Buffer) { GetMemoryMapStatus = EFI_OUT_OF_RESOURCES; }
    return GetMemoryMapStatus;
}

void SwitchToResolution(unsigned int x, unsigned int y)
{
    unsigned long long                    sizeofInfo = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    for (unsigned int i = 0; i < GOP->Mode->MaxMode; i++)
    {
        GOP->QueryMode(GOP, i, &sizeofInfo, &info);

        if (info->HorizontalResolution == x && info->VerticalResolution == y) { GOP->SetMode(GOP, i); }
    }
}

// 比较GUID
UINTN CompareGUID(EFI_GUID *src, EFI_GUID *obj)
{
    if (src->Data1 == obj->Data1 && src->Data2 == obj->Data2 && src->Data3 == obj->Data3 &&
        src->Data4[0] == obj->Data4[0] && src->Data4[1] == obj->Data4[1] && src->Data4[2] == obj->Data4[2] &&
        src->Data4[3] == obj->Data4[3] && src->Data4[4] == obj->Data4[4] && src->Data4[5] == obj->Data4[5] &&
        src->Data4[6] == obj->Data4[6] && src->Data4[7] == obj->Data4[7])
    {
        return 1; // Yes!
    }
    return 0;
}

#define PAGE_TABLE_MAPPED_ADDRESS 0xFFFFFFFFFFFFF000

void CreateAndMapNewPageTable()
{
    UINT64 new_page_table_addr;
    BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &new_page_table_addr);

    UINT64 old_page_table_addr;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_page_table_addr));

    xmemcpy((void *)new_page_table_addr, (void *)old_page_table_addr, PAGE_SIZE);

    UINT64 *pml4 = (UINT64 *)new_page_table_addr;
    UINT64 *pdp  = (UINT64 *)(pml4[0] & ~0xFFF);
    pml4[0]      = (UINT64)pdp | 0x7;
    pml4[256]    = (UINT64)pdp | 0x3;

    __asm__ __volatile__("mov %0, %%cr3" : : "r"(new_page_table_addr));
}

void MapVirtToPhys(UINT64 virt, UINT64 size)
{
    UINT64 page_table_addr;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(page_table_addr));

#define KERNEL_PERM 0x03

    for (UINT64 addr = virt; addr < virt + size + PAGE_SIZE - 1; addr += PAGE_SIZE)
    {
        // Map vaddr to paddr
        UINT64 *pml4    = (UINT64 *)page_table_addr;
        UINT64  pml4_id = (addr >> 39) & 0x1FF;
        if (pml4[pml4_id] == 0)
        {
            UINT64 pml4_addr;
            BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &pml4_addr);
            xmemset((void *)pml4_addr, 0, PAGE_SIZE);
            pml4[pml4_id] = pml4_addr | KERNEL_PERM;
        }
        UINT64 *pdpt    = (UINT64 *)(pml4[pml4_id] & ~(0xFFF));
        UINT64  pdpt_id = (addr >> 30) & 0x1FF;
        if (pdpt[pdpt_id] == 0)
        {
            UINT64 pdpt_addr;
            BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &pdpt_addr);
            xmemset((void *)pdpt_addr, 0, PAGE_SIZE);
            pdpt[pdpt_id] = pdpt_addr | KERNEL_PERM;
        }
        UINT64 *pd    = (UINT64 *)(pdpt[pdpt_id] & ~(0xFFF));
        UINT64  pd_id = (addr >> 21) & 0x1FF;
        if (pd[pd_id] == 0)
        {
            UINT64 pd_addr;
            BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &pd_addr);
            xmemset((void *)pd_addr, 0, PAGE_SIZE);
            pd[pd_id] = pd_addr | KERNEL_PERM;
        }
        UINT64 *pt    = (UINT64 *)(pd[pd_id] & ~(0xFFF));
        UINT64  pt_id = (addr >> 12) & 0x1FF;
        if (pt[pt_id] == 0)
        {
            UINT64 paddr;
            BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &paddr);
            xmemset((void *)paddr, 0, PAGE_SIZE);
            pt[pt_id] = paddr | KERNEL_PERM;
        }
    }
}

int check_qemu(char *acpi_oem_id)
{
    if (acpi_oem_id[0] == 'B' && acpi_oem_id[1] == 'O' && acpi_oem_id[2] == 'C' && acpi_oem_id[3] == 'H' &&
        acpi_oem_id[4] == 'S')
    {
        return 1;
    }
    return 0;
}

void asm_cpuid(UINT32 mop, UINT32 sop, UINT32 *eax, UINT32 *ebx, UINT32 *ecx, UINT32 *edx)
{
    __asm__ volatile("cpuid \n\t" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "0"(mop), "2"(sop) : "memory");
}

char new_stack[STACK_SIZE]; // 你不配当主播！滚吧！* 哭 *

typedef void (*__attribute__((sysv_abi)) Kernel)(const struct FrameBufferConfig *, struct EFI_SYSTEM_TABLE *,
                                                 BOOT_CONFIG *);

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, struct EFI_SYSTEM_TABLE *SystemTable)
{
    write_serial_string("\n");
    write_serial_string("XJ380 Operating System - Serial Log\n");
    write_serial_string("Copyright(C) XINGJI Interacitve Software 2017-2026 All rights reserved.\n");
    write_serial_string("OS Version: BuildVersion\n");
    efi_init(ImageHandle, SystemTable);
    write_serial_string("EFI Initialize Success.\n");
    boot_menu_init_defaults();
    boot_status("Initializing firmware services");
    if (wait_for_boot_menu_key())
    {
        write_serial_string("Boot Menu Entered.\n");
        boot_menu();
    }
    boot_status("Preparing boot environment");

    CreateAndMapNewPageTable();
    write_serial_string("New Page Table Created Success.\n");
    boot_status("Loading kernel");

    // ST->ConOut->ClearScreen(ST->ConOut);

    EFI_STATUS                status;
    EFI_PHYSICAL_ADDRESS      entry_addr;
    struct EFI_FILE_PROTOCOL *root;
    UINTN                     kernel_size   = 0;
    void                     *kernel_buffer = NULL;
    UINTN                     installer_root_size = 0;
    UINTN                     system_payload_size = 0;
    void                     *installer_root_buffer = NULL;
    void                     *system_payload_buffer = NULL;
    int                       installer_mode = 0;

    // 引导内核
    status = SFSP->OpenVolume(SFSP, &root);

    if (EFI_ERROR(status))
    {
        puts(L"XJ380 does not boot properly. Please visit "
             L"https://xingjisoft.com/os/xj380/error. Error code: OPEN_ROOT_DIR_ERROR");
        write_serial_string("Root Dir Open Failed. Stop.\n");
        while (1)
            ;
    }
    write_serial_string("Root Dir Open Success.\n");
    boot_status("Opening kernel image");

    status = read_boot_file(root, L"\\installer\\installer-kernel.elf", &kernel_buffer, &kernel_size);
    if (!EFI_ERROR(status))
    {
        installer_mode = 1;
        BootMenuFlags |= BOOT_FLAG_INSTALLER | BOOT_FLAG_SHOW_PROGRESS | BOOT_FLAG_DISABLE_KMOD |
                         BOOT_FLAG_SAFE_STORAGE_IO;
        write_serial_string("Installer kernel found.\n");
        boot_status("Loading installer payloads");

        status = read_boot_file(root, L"\\installer\\installer-root.pak", &installer_root_buffer,
                                &installer_root_size);
        if (EFI_ERROR(status))
        {
            puts(L"XJ380 installer payload is missing. Error code: INSTALLER_ROOT_NOT_FOUND");
            write_serial_string("Installer root payload read failed. Stop.\n");
            while (1)
                ;
        }

        status = read_boot_file(root, L"\\installer\\system-payload.pak", &system_payload_buffer,
                                &system_payload_size);
        if (EFI_ERROR(status))
        {
            puts(L"XJ380 installer payload is missing. Error code: SYSTEM_PAYLOAD_NOT_FOUND");
            write_serial_string("System payload read failed. Stop.\n");
            while (1)
                ;
        }
    }
    else
    {
        root->Close(root);
        root = NULL;

        status = open_system_volume(&root);
        if (EFI_ERROR(status))
        {
            write_serial_string("system not found\n");
            while (1)
                ;
        }

        status = read_boot_file(root, L"\\system\\kernel.krl", &kernel_buffer, &kernel_size);
    }

    if (EFI_ERROR(status))
    {
        puts(L"XJ380 does not boot properly. Please visit "
             L"https://xingjisoft.com/os/xj380/error. Error code: KERNEL_NOT_FOUND");
        write_serial_string("Kernel Read Failed. Stop.\n");
        while (1)
            ;
    }

    write_serial_string("XSK2.1 Kernel Read Success.\n");
    boot_status("Mapping kernel image");

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)kernel_buffer;
    UINT64      kernel_first_addr, kernel_last_addr;                   // 计算的首尾
    CalcLoadAddressRange(ehdr, &kernel_first_addr, &kernel_last_addr); // 计算范围

    MapVirtToPhys(kernel_first_addr, kernel_last_addr - kernel_first_addr);

    xmemset((void *)kernel_first_addr, 0, kernel_last_addr - kernel_first_addr);

    CopyLoadSegments(ehdr);     // CV
    entry_addr = ehdr->e_entry; // 获取入口点
    // free(kernel_buffer);        // 释放内核文件，丢掉！
    boot_status("Initializing video");

    if ((BootMenuFlags & BOOT_FLAG_BASE_VIDEO) != 0)
    {
        SwitchToResolution(800, 600);
    }
#if BUILD_EDITION == DEBUG_VERSION
    else
    {
        SwitchToResolution(1280, 768); // 切换分辨率
    }
#endif

    struct FrameBufferConfig config = {(UINT8 *)(GOP->Mode->FrameBufferBase + 0xffff800000000000),
                                       GOP->Mode->Info->PixelsPerScanLine, GOP->Mode->Info->HorizontalResolution,
                                       GOP->Mode->Info->VerticalResolution, kRGBR};
    write_serial_string("Frame Buffer Phys Addr: ");
    write_serial_hex((UINT64)(config.frame_buffer));
    write_serial_string("\n");

    switch (GOP->Mode->Info->PixelFormat)
    {
    case PixelRedGreenBlueReserved8BitPerColor: config.pixel_format = kRGBR; break;
    case PixelBlueGreenRedReserved8BitPerColor: config.pixel_format = kBGRR; break;
    default:

        puts(L"XJ380 does not boot properly. Please visit "
             L"https://xingjisoft.com/os/xj380/error. Error code: "
             L"UNSUPPORTED_PIXEL_FORMAT");
        write_serial_string("Video Initialize Failed. Stop.\n");
        while (1)
            ;
    }
    write_serial_string("Video Initialize Success.\n");
    boot_status("Loading ACPI");

    BOOT_CONFIG *BootConfig;
    BootConfig = malloc(sizeof(BOOT_CONFIG)) + 0xFFFF800000000000;
    xmemset(BootConfig, 0, sizeof(BOOT_CONFIG));

    BootConfig->is_qemu = 0;
    BootConfig->boot_flags = BootMenuFlags;
    if (installer_mode)
    {
        BootConfig->installer_root_pak = (UINT64)installer_root_buffer;
        BootConfig->installer_root_pak_size = (UINT64)installer_root_size;
        BootConfig->system_payload_pak = (UINT64)system_payload_buffer;
        BootConfig->system_payload_pak_size = (UINT64)system_payload_size;
    }

    // 获取RSDP
    EFI_CONFIGURATION_TABLE *SystemConfigTable = NULL;
    UINT8                    AcpiTableWasFound = 0; // 0=找不着 1=找到了
    SystemConfigTable                          = ST->ConfigurationTable;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++)
    {
        if (CompareGUID(&SystemConfigTable->VendorGuid, &acpi_table_guid) == 1 ||
            CompareGUID(&SystemConfigTable->VendorGuid, &efi_acpi_table_guid) == 1)
        {
            // 找到了ACPI表
            // 修订版 >= ACPI 2.0
            RSDP_TYPE *AcpiRsdp = (RSDP_TYPE *)(SystemConfigTable->VendorTable); // 指向存储RSDP的表的指针
            // 修订版 >= ACPI 2.0
            if (AcpiRsdp->version >= 2)
            {
                write_serial_string("ACPI Table Found Success.\n");
                write_serial_string("ACPI Version: ");
                write_serial_dec(AcpiRsdp->version);
                write_serial_string("\n");
                write_serial_string("XSDT Address: ");
                write_serial_hex(AcpiRsdp->XsdtAddr);
                write_serial_string("\n");
                BootConfig->is_qemu = check_qemu(AcpiRsdp->OEMID);

                ACPI_TABLE_HEADER *XSDT              = (ACPI_TABLE_HEADER *)(AcpiRsdp->XsdtAddr); // 指向XSDT的指针
                UINT64            *AcpiTableEntryPtr = (UINT64 *)(XSDT + 1); // 指向XSDT后面的其他SDT的地址的指针
                UINT64             Entries           = (XSDT->length - sizeof(ACPI_TABLE_HEADER)) / 8; // SDT数量
                AcpiTableWasFound                    = 1;
                UINT8 ObjTableWasFound               = 0;
                // 遍历所有ACPI表
                for (UINT64 j = 0; j < Entries; j++, AcpiTableEntryPtr++)
                {
                    write_serial_string("SDT Entry ");
                    write_serial_dec(j);
                    write_serial_string(":\n");

                    ACPI_TABLE_HEADER *AcpiSdtPointer =
                        (ACPI_TABLE_HEADER *)(*AcpiTableEntryPtr); // 指向其他SDT的表头的指针

                    write_serial_string("SDT Address: ");
                    write_serial_hex((UINT64)(AcpiTableEntryPtr));
                    write_serial_string("\n");
                    write_serial_string(AcpiSdtPointer->sign);
                    write_serial_string("\n");

                    if (!part_strcmp(AcpiSdtPointer->sign, "APIC", 4))
                    {
                        write_serial_string("MADT Found Success.\n");
                        // MADT!!!
                        BootConfig->MADT = (UINT64)(AcpiSdtPointer);
                        ObjTableWasFound++;
                    }
                    else if (!part_strcmp(AcpiSdtPointer->sign, "FACP", 4))
                    {
                        write_serial_string("FADT Found Success.\n");
                        BootConfig->FADT = (UINT64)(AcpiSdtPointer);
                        ObjTableWasFound++;
                    }
                    else if (!part_strcmp(AcpiSdtPointer->sign, "HPET", 4))
                    {
                        write_serial_string("HPET Found Success.\n");
                        // HPET!!!
                        BootConfig->HPET = (UINT64)(AcpiSdtPointer);
                        ObjTableWasFound++;
                    }
                    else if (!part_strcmp(AcpiSdtPointer->sign, "MCFG", 4))
                    {
                        write_serial_string("MCFG Found Success.\n");
                        // MCFG!!!
                        BootConfig->MCFG = (UINT64)(AcpiSdtPointer);
                        ObjTableWasFound++;
                    }
                    write_serial_string("\n");
                }
                break;
            }
        }
        SystemConfigTable++;
    }
    boot_status("Preparing memory map");

    if (AcpiTableWasFound == 0)
    {
        puts(L"XJ380 does not boot properly. Please visit "
             L"https://xingjisoft.com/os/xj380/error. Error code: "
             L"ACPI_TABLE_NOT_FOUND\r\n");
        write_serial_string("ACPI Table Not Found. Stop.\n");
        while (1)
            ;
    }
    if (BootConfig->MADT == 0)
    {
        puts(L"XJ380 does not boot properly. Please visit "
             L"https://xingjisoft.com/os/xj380/error. Error code: "
             L"MADT_NOT_FOUND\r\n");
        write_serial_string("MADT Not Found. Stop.\n");
        while (1)
            ;
    }

    // 获取memory map
    BootConfig->MemoryMap.MapSize           = 4096;
    BootConfig->MemoryMap.Buffer            = NULL;
    BootConfig->MemoryMap.MapKey            = 0;
    BootConfig->MemoryMap.DescriptorSize    = 0;
    BootConfig->MemoryMap.DescriptorVersion = 0;

    BootConfig->saved_mtrrs = NULL;
    UINT32 eax, ebx, ecx, edx;
    asm_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!!(edx & (1 << 12)))
    {
        UINT64 ia32_mtrrcap  = rdmsr(0xfe);
        UINT8  var_reg_count = ia32_mtrrcap & 0xff;
        if (BootConfig->saved_mtrrs == NULL)
        {
            BootConfig->saved_mtrrs = malloc(((var_reg_count * 2) /* variable MTRRs, 2 MSRs each */
                                              + 11                /* 11 fixed MTRRs */
                                              + 1                 /* 1 default type MTRR */
                                              ) *
                                             sizeof(UINT64));
            xmemset(BootConfig->saved_mtrrs,0,((var_reg_count * 2) /* variable MTRRs, 2 MSRs each */
                                              + 11                /* 11 fixed MTRRs */
                                              + 1                 /* 1 default type MTRR */
                                              ) *
                                             sizeof(UINT64));
        }
    }

    for (int i = 0; i < 256; i++)
    {
        BootConfig->temp_stack[i] = malloc(STACK_SIZE) + 0xFFFF800000000000;
        xmemset(BootConfig->temp_stack[i],0,STACK_SIZE);
    }

    GetMMP(&BootConfig->MemoryMap);
    boot_status("Exiting boot services");

    __asm__ volatile("mfence" ::: "memory");
    __asm__ volatile("mov %0, %%rsp \n\t" : : "r"(new_stack + STACK_SIZE + 0xFFFF800000000000) : "memory");
    __asm__ volatile("mfence" ::: "memory");

    EFI_STATUS ExitBSStatus = EFI_SUCCESS;

    // say goodbye~
    ExitBSStatus = BS->ExitBootServices(ImageHandle, BootConfig->MemoryMap.MapKey);
    if (EFI_ERROR(ExitBSStatus))
    {
        GetMMP(&BootConfig->MemoryMap);
        ExitBSStatus = BS->ExitBootServices(ImageHandle, BootConfig->MemoryMap.MapKey);
    }

    if (EFI_ERROR(ExitBSStatus))
    {
        puts(L"XJ380 does not boot properly. Please visit "
             L"https://xingjisoft.com/os/xj380/error. Error code: "
             L"CANNOT_BOOT_SYSTEM");
        write_serial_string("Boot Services Exit Failed. Stop.\n");
        while (1)
            ;
    }

    Kernel kernel = (Kernel)entry_addr;
    kernel((struct FrameBufferConfig *)((UINT64)&config + 0xFFFF800000000000), SystemTable, BootConfig); // 滚进去！

    // 此处=kernel.krl

    while (1)
        ; // 趋势
}
