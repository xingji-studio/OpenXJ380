#include <graphics/window/window.h>
#include <graphics/GOP.hpp>
#include <errno.h>
#include <syscall/pxapi.h>

XWM_INFO *xwmii = nullptr;
SHEET_INFO *sht_img = nullptr;

void delete_process_windows(XWM_INFO *xwmi, SHEET_INFO *sht, pcb_t process)
{
    (void)xwmi;
    (void)sht;
    (void)process;
}

WINDOWLS *mpf_found_win(MsgPrcor mpf)
{
    (void)mpf;
    return nullptr;
}

void toast_manager_mark_process_exit(pcb_t process)
{
    (void)process;
}

extern "C" void c_mouse_handler(void *regs_ptr, uint64_t error_code)
{
    (void)regs_ptr;
    (void)error_code;
}

extern "C" void mouse_inject_report(int dx, int dy, uint8_t buttons, int wheel)
{
    (void)dx;
    (void)dy;
    (void)buttons;
    (void)wheel;
}

void do_xapi_Broken(char *info)
{
    (void)info;
}

uint64_t do_xapi_KillProcess(uint64_t pid)
{
    (void)pid;
    return (uint64_t)-ENOSYS;
}

uint64_t do_xapi_SendAppMessage(char *title, char *text)
{
    (void)title;
    (void)text;
    return (uint64_t)-ENOSYS;
}

uint64_t do_xapi_InstallerEnumDisks(uint64_t list) { (void)list; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerStart(uint64_t disk_id) { (void)disk_id; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerStartEx(uint64_t disk_id, uint64_t mode) { (void)disk_id; (void)mode; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerStartOptions(uint64_t options) { (void)options; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerPrecheck(uint64_t disk_id, uint64_t mode, uint64_t out) { (void)disk_id; (void)mode; (void)out; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerPrecheckOptions(uint64_t options, uint64_t out) { (void)options; (void)out; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerProgress(uint64_t progress) { (void)progress; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerRescue(uint64_t action, uint64_t disk_id, uint64_t out) { (void)action; (void)disk_id; (void)out; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerLog(uint64_t out) { (void)out; return (uint64_t)-ENOSYS; }

void rect(const FrameBufferConfig &fbc, int x1, int y1, int x2, int y2, const PixelColor &color)
{
    if (fbc.frame_buffer == nullptr) return;
    for (int y = y1 < y2 ? y1 : y2; y <= (y1 < y2 ? y2 : y1); y++)
    {
        if (y < 0 || y >= (int)fbc.vertical_resolution) continue;
        for (int x = x1 < x2 ? x1 : x2; x <= (x1 < x2 ? x2 : x1); x++)
        {
            if (x < 0 || x >= (int)fbc.horizontal_resolution) continue;
            uint8_t *pixel = PixelAt(x, y, fbc);
            if (fbc.pixel_format == PixelFormat::kRGBR) {
                pixel[0] = color.r;
                pixel[1] = color.g;
                pixel[2] = color.b;
            } else {
                pixel[0] = color.b;
                pixel[1] = color.g;
                pixel[2] = color.r;
            }
        }
    }
}

void WriteAscii(const FrameBufferConfig &fbc, int x, int y, char ch, const PixelColor &color)
{
    (void)ch;
    rect(fbc, x, y, x + 7, y + 15, color);
}
