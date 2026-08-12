#include <cpu/regio.h>
#include <openxj380/config.h>
#include <openxj380/socket.h>
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

static void mouse_apply_report(int dx, int dy, uint8_t buttons, int wheel)
{
    g_mouse.buttons = buttons & 0x07;
    g_mouse.x += dx;
    g_mouse.y += dy;
    g_mouse.scroll += wheel;
}

static void mouse_apply_packet()
{
    int dx = (int8_t)g_mouse.buf[1];
    int dy = -(int)(int8_t)g_mouse.buf[2];
    mouse_apply_report(dx, dy, g_mouse.buf[0] & 0x07, 0);
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
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380MouseInterruptInfo event = {};
    event.source = OPENXJ380_INPUT_SOURCE_USB;
    event.route = OPENXJ380_INPUT_ROUTE_XJ380_USB;
    event.packet_complete = 1;
    event.buttons = buttons;
    event.delta_x = (int16_t)dx;
    event.delta_y = (int16_t)dy;
    event.wheel = (int16_t)wheel;
    OpenXJ380Socket_MouseInterrupte(&event);
#else
    mouse_apply_report(dx, dy, buttons, wheel);
    OpenXJ380MouseInterruptInfo event = {};
    event.source = OPENXJ380_INPUT_SOURCE_USB;
    event.packet_complete = 1;
    event.buttons = g_mouse.buttons;
    event.delta_x = (int16_t)dx;
    event.delta_y = (int16_t)dy;
    event.wheel = (int16_t)wheel;
    event.x = g_mouse.x;
    event.y = g_mouse.y;
    OpenXJ380Socket_MouseInterrupte(&event);
#endif
}

extern "C" void c_mouse_handler(void *regs_ptr, uint64_t error_code)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380MouseInterruptInfo event = {};
    event.regs = regs_ptr;
    event.error_code = error_code;
    event.source = OPENXJ380_INPUT_SOURCE_PS2;
    event.route = OPENXJ380_INPUT_ROUTE_XJ380_PS2_IRQ;
    if (!OpenXJ380Socket_MouseInterrupte(&event)) send_eoi();
    return;
#else
    uint8_t packet_byte = inb(PS2_DATA_PORT);
    int old_x = g_mouse.x;
    int old_y = g_mouse.y;
    int old_scroll = g_mouse.scroll;
    bool packet_complete = mousedecode(packet_byte);
    OpenXJ380MouseInterruptInfo event = {};
    event.regs = regs_ptr;
    event.error_code = error_code;
    event.source = OPENXJ380_INPUT_SOURCE_PS2;
    event.packet_byte = packet_byte;
    event.packet_complete = packet_complete ? 1 : 0;
    event.buttons = g_mouse.buttons;
    event.delta_x = (int16_t)(g_mouse.x - old_x);
    event.delta_y = (int16_t)(g_mouse.y - old_y);
    event.wheel = (int16_t)(g_mouse.scroll - old_scroll);
    event.x = g_mouse.x;
    event.y = g_mouse.y;
    OpenXJ380Socket_MouseInterrupte(&event);
    send_eoi();
#endif
}

void mouse_init()
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    g_mouse = {};
    mouse_wait_write();
    outb(PS2_CMD_PORT, KB_EN_MOUSE_INTFACE);
    mouse_write(MOUSE_EN);
    g_mouse.phase = 0;
#endif
}

int get_mouse_x()
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    return 0;
#else
    return g_mouse.x;
#endif
}

int get_mouse_y()
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    return 0;
#else
    return g_mouse.y;
#endif
}

int get_mouse_scroll()
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    return 0;
#else
    int scroll = g_mouse.scroll;
    g_mouse.scroll = 0;
    return scroll;
#endif
}

void set_mouse_position(int x, int y)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)x;
    (void)y;
#else
    g_mouse.x = x;
    g_mouse.y = y;
#endif
}
