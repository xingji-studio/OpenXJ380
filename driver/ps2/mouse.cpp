#include <cpu/regio.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>

static mouse_dec g_mouse = {};

static void mouse_wait_write()
{
    for (size_t i = 0; i < MAX_WAIT_INDEX; ++i)
    {
        if ((inb(PS2_CMD_PORT) & KB_STATUS_IBF) == 0) return;
    }
}

static void mouse_write(uint8_t value)
{
    mouse_wait_write();
    outb(PS2_CMD_PORT, KB_SEND2MOUSE);
    mouse_wait_write();
    outb(PS2_DATA_PORT, value);
}

static void mouse_apply_packet()
{
    int dx = (int8_t)g_mouse.buf[1];
    int dy = -(int)(int8_t)g_mouse.buf[2];
    mouse_inject_report(dx, dy, g_mouse.buf[0] & 0x07, 0);
}

bool mousedecode(uint8_t data)
{
    if (g_mouse.phase == 0)
    {
        if (data == 0xfa) g_mouse.phase = 1;
        return false;
    }

    if (g_mouse.phase == 1)
    {
        if ((data & 0x08) == 0) return false;
        g_mouse.buf[0] = data;
        g_mouse.phase  = 2;
        return false;
    }

    if (g_mouse.phase == 2)
    {
        g_mouse.buf[1] = data;
        g_mouse.phase  = 3;
        return false;
    }

    g_mouse.buf[2] = data;
    g_mouse.phase  = 1;
    mouse_apply_packet();
    return true;
}

extern "C" void mouse_inject_report(int dx, int dy, uint8_t buttons, int wheel)
{
    g_mouse.buttons = buttons & 0x07;
    g_mouse.x += dx;
    g_mouse.y += dy;
    g_mouse.scroll += wheel;
}

extern "C" void c_mouse_handler(void *regs_ptr, uint64_t error_code)
{
    (void)regs_ptr;
    (void)error_code;
    mousedecode(inb(PS2_DATA_PORT));
    send_eoi();
}

void mouse_init()
{
    g_mouse = {};
    mouse_wait_write();
    outb(PS2_CMD_PORT, KB_EN_MOUSE_INTFACE);
    mouse_write(MOUSE_EN);
    g_mouse.phase = 0;
}

int get_mouse_x()
{
    return g_mouse.x;
}

int get_mouse_y()
{
    return g_mouse.y;
}

int get_mouse_scroll()
{
    int scroll = g_mouse.scroll;
    g_mouse.scroll = 0;
    return scroll;
}

void set_mouse_position(int x, int y)
{
    g_mouse.x = x;
    g_mouse.y = y;
}
