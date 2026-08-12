#include <proto.hpp>
#include <atom_queue.h>
#include <dlinker.h>
#include <openxj380/config.h>
#include <openxj380/socket.h>
#include <syscall/syscall.h>

struct keyboard_buf kb_fifo;
static atom_queue kb_fifo_queue;
static volatile bool kb_fifo_ready = false;
static uint8_t kb_synth_buf[KB_BUF_SIZE];
static uint8_t *kb_synth_head = kb_synth_buf;
static uint8_t *kb_synth_tail = kb_synth_buf;
static volatile uint32_t kb_synth_lock = 0;
static bool kb_e0_prefix = false;
static uint8_t kb_ps2_pressed_values[2][128];
static uint8_t kb_usb_pressed_values[256];
extern uint8_t keyboard_code[256];
extern uint8_t keyboard_code1[256];

static void keyboard_emit_socket(void *regs_ptr, uint64_t error_code, uint8_t raw_scancode, uint8_t make_code,
                                  uint8_t value, bool extended, bool pressed)
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380KeyboardInterruptInfo event = {};
    event.regs = regs_ptr;
    event.error_code = error_code;
    event.source = OPENXJ380_INPUT_SOURCE_PS2;
    event.raw_scancode = raw_scancode;
    event.make_code = make_code;
    event.value = value;
    event.message_type = (uint8_t)(pressed ? MSG_KEYDOWN : MSG_KEYUP);
    event.extended = extended ? 1 : 0;
    event.pressed = pressed ? 1 : 0;
    event.shift = kb_fifo.shift ? 1 : 0;
    event.ctrl = kb_fifo.ctrl ? 1 : 0;
    event.alt = kb_fifo.alt ? 1 : 0;
    event.win = kb_fifo.win ? 1 : 0;
    event.caps = kb_fifo.caps ? 1 : 0;
    OpenXJ380Socket_KeyboardInterrupt(&event);
#else
    (void)regs_ptr;
    (void)error_code;
    (void)raw_scancode;
    (void)make_code;
    (void)value;
    (void)extended;
    (void)pressed;
#endif
}

#define KB_USB_REPEAT_SLOTS 6
#define KB_USB_REPEAT_DELAY_NS 500000000ULL
#define KB_USB_REPEAT_INTERVAL_NS 25000000ULL
struct keyboard_usb_repeat_slot
{
    bool active;
    uint8_t usage;
    uint8_t value;
    uint64_t next_repeat_ns;
};

static keyboard_usb_repeat_slot kb_usb_repeat[KB_USB_REPEAT_SLOTS];

static void kb_synth_lock_acquire()
{
    while (__sync_lock_test_and_set(&kb_synth_lock, 1) != 0)
    {
        while (kb_synth_lock != 0) { __asm__ volatile("pause"); }
    }
}

static void kb_synth_lock_release()
{
    __sync_lock_release(&kb_synth_lock);
}

static bool kb_value_repeatable(uint8_t value)
{
    return value == '\b' || value == '\n' || (value >= 32 && value < 127);
}

static void kb_synth_enqueue_locked(uint8_t value)
{
    if (value == 0) return;

    uint8_t *next = kb_synth_head + 1;
    if (next == kb_synth_buf + KB_BUF_SIZE) { next = kb_synth_buf; }

    if (next == kb_synth_tail) { return; }

    *kb_synth_head = value;
    kb_synth_head  = next;
}

static void keyboard_prepare_fifo()
{
    if (kb_fifo_ready) return;

    kb_fifo.p_head = kb_fifo.buf;
    kb_fifo.p_tail = kb_fifo.buf;
    kb_fifo.count  = 0;
    kb_fifo.ctrl   = false;
    kb_fifo.shift  = false;
    kb_fifo.alt    = false;
    kb_fifo.win    = false;
    kb_fifo.caps   = false;
    memset(kb_fifo.buf, 0, KB_BUF_SIZE);
    init_atom_queue(&kb_fifo_queue, kb_fifo.buf, KB_BUF_SIZE);
    kb_synth_head  = kb_synth_buf;
    kb_synth_tail  = kb_synth_buf;
    memset(kb_synth_buf, 0, sizeof(kb_synth_buf));
    memset(kb_ps2_pressed_values, 0, sizeof(kb_ps2_pressed_values));
    memset(kb_usb_pressed_values, 0, sizeof(kb_usb_pressed_values));
    memset(kb_usb_repeat, 0, sizeof(kb_usb_repeat));
    kb_fifo_ready = true;
}

static void kb_synth_enqueue(uint8_t value)
{
    kb_synth_lock_acquire();
    kb_synth_enqueue_locked(value);
    kb_synth_lock_release();
}

static uint8_t kb_synth_dequeue()
{
    uint8_t value = 0;

    kb_synth_lock_acquire();
    if (kb_synth_tail != kb_synth_head)
    {
        value = *kb_synth_tail;
        kb_synth_tail++;
        if (kb_synth_tail == kb_synth_buf + KB_BUF_SIZE) {
            kb_synth_tail = kb_synth_buf;
        }
    }
    kb_synth_lock_release();
    return value;
}

static void kb_usb_repeat_service()
{
    uint64_t now = nanoTime();
    if (now == 0) return;

    kb_synth_lock_acquire();
    for (size_t i = 0; i < KB_USB_REPEAT_SLOTS; ++i)
    {
        keyboard_usb_repeat_slot *slot = &kb_usb_repeat[i];
        if (!slot->active || now < slot->next_repeat_ns) { continue; }

        kb_synth_enqueue_locked(slot->value);
        slot->next_repeat_ns = now + KB_USB_REPEAT_INTERVAL_NS;
    }
    kb_synth_lock_release();
}

static uint8_t keyboard_translate_base_make_code(uint8_t make_code, bool shift,
                                                 bool caps)
{
    if (make_code >= 128) {
        return 0;
    }

    uint8_t base = keyboard_code[make_code];
    if (base == 0) {
        return 0;
    }

    if (base >= 'a' && base <= 'z')
    {
        if (shift ^ caps) {
            return (uint8_t)(base - 'a' + 'A');
        }
        return base;
    }

    return shift ? keyboard_code1[make_code] : base;
}

uint8_t keyboard_translate_extended_make_code(uint8_t make_code)
{
    switch (make_code)
    {
    case 0x1c: return '\n';
    case 0x1d: return KEY_CTRL;
    case 0x35: return '/';
    case 0x38: return KEY_ALT;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PAGE_UP;
    case 0x4b: return KEY_LEFT;
    case 0x4d: return KEY_RIGHT;
    case 0x4f: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PAGE_DOWN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    default: return 0;
    }
}

uint8_t keyboard_translate_event_value(uint8_t make_code, bool extended,
                                       bool shift, bool caps)
{
    if (extended) {
        return keyboard_translate_extended_make_code(make_code);
    }
    return keyboard_translate_base_make_code(make_code, shift, caps);
}

extern "C" void keyboard_dispatch_key_message(uint64_t msg_type, uint8_t value)
{
    (void)msg_type;
    (void)value;
}

static void keyboard_update_modifier_state(uint8_t make_code, bool extended, bool pressed)
{
    (void)extended;
    switch (make_code)
    {
    case 0x2a:
    case 0x36: kb_fifo.shift = pressed; break;
    case 0x1d: kb_fifo.ctrl = pressed; break;
    case 0x38: kb_fifo.alt = pressed; break;
    case 0x3a: if (pressed) kb_fifo.caps = !kb_fifo.caps; break;
    default: break;
    }
}

/*
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25， 26, 27, 28,
    29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68,
    69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88
*/

uint8_t keyboard_code[256] = {
    0, KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    KEY_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    KEY_CTRL, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    KEY_SHIFT, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', KEY_SHIFT,
    '*', KEY_ALT, ' ', KEY_CAPS, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_NUML, KEY_SCROLL, '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0, KEY_F11, KEY_F12}; // +0x80 = 释放状态
uint8_t keyboard_code1[256] = {                                                                                          // 按下Shift
    0, KEY_ESC, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    KEY_TAB, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    KEY_CTRL, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    KEY_SHIFT, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', KEY_SHIFT,
    '*', KEY_ALT, ' ', KEY_CAPS, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_NUML, KEY_SCROLL, '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0, KEY_F11, KEY_F12};

extern "C" void c_keyboard_handler(void *regs_ptr, uint64_t error_code)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380KeyboardInterruptInfo event = {};
    event.regs = regs_ptr;
    event.error_code = error_code;
    event.source = OPENXJ380_INPUT_SOURCE_PS2;
    event.route = OPENXJ380_INPUT_ROUTE_XJ380_PS2_IRQ;
    if (!OpenXJ380Socket_KeyboardInterrupt(&event)) send_eoi();
    return;
#else
    keyboard_prepare_fifo();

    uint8_t x = inb(0x60);

    if (x == 0xe0)
    {
        kb_e0_prefix = true;
        keyboard_emit_socket(regs_ptr, error_code, x, 0, 0, true, true);
        send_eoi();
        return;
    }

    bool extended = kb_e0_prefix;
    bool pressed = (x & 0x80) == 0;
    bool key_release = !pressed;
    uint8_t make_code = pressed ? x : (uint8_t)(x & 0x7f);

    uint8_t event_value = 0;
    if (make_code < 128)
    {
        uint8_t state_index = extended ? 1 : 0;
        if (pressed)
        {
            event_value = keyboard_translate_event_value(
                make_code,
                extended,
                kb_fifo.shift,
                kb_fifo.caps
            );
            kb_ps2_pressed_values[state_index][make_code] = event_value;
        }
        else
        {
            event_value = kb_ps2_pressed_values[state_index][make_code];
            if (event_value == 0)
            {
                event_value = keyboard_translate_event_value(
                    make_code,
                    extended,
                    kb_fifo.shift,
                    kb_fifo.caps
                );
            }
            kb_ps2_pressed_values[state_index][make_code] = 0;
        }
    }

    keyboard_dispatch_key_message(
        pressed ? MSG_KEYDOWN : MSG_KEYUP,
        event_value
    );

    keyboard_update_modifier_state(make_code, extended, pressed);

    keyboard_emit_socket(regs_ptr, error_code, x, make_code, event_value, extended, pressed);

    kb_e0_prefix = false;

    if (key_release)
    {
        send_eoi();
        return;
    }

    uint8_t fifo_value = event_value;

    if (fifo_value == 0)
    {
        send_eoi();
        return;
    }

    if (!atom_push(&kb_fifo_queue, fifo_value))
    {
        atom_pop(&kb_fifo_queue);
        atom_push(&kb_fifo_queue, fifo_value);
    }

    send_eoi();
#endif
}

uint8_t get_keyboard_input()
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    return 0;
#else
    keyboard_prepare_fifo();
    kb_usb_repeat_service();

    uint8_t synthetic = kb_synth_dequeue();
    if (synthetic != 0) {
        return synthetic;
    }

    int raw_input = atom_pop(&kb_fifo_queue);
    if (raw_input >= 0) { return (uint8_t)raw_input; }
    return NULL;
#endif
}

void wait_ps2_write()
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    for (size_t i = 0; i < MAX_WAIT_INDEX; ++i)
    {
        if (!(inb(PS2_CMD_PORT) & KB_STATUS_IBF)) return;
    }
#endif
}

void wait_ps2_read()
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    for (size_t i = 0; i < MAX_WAIT_INDEX; ++i)
    {
        if (!(inb(PS2_CMD_PORT) & KB_STATUS_OBF)) return;
    }
#endif
}

void keyboard_init()
{
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
    keyboard_prepare_fifo();
    wait_ps2_write();
    outb(PORT_KB_CMD, KBCMD_WRITE_CMD);
    wait_ps2_read();
    outb(PORT_KB_DATA, KB_INIT_MODE);
#endif
}

extern "C" void keyboard_push_input(uint8_t value)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    (void)value;
#else
    kb_synth_enqueue(value);
#endif
}

extern "C" void keyboard_usb_key_event(uint8_t usage, uint8_t value, uint8_t pressed)
{
#if OPENXJ380_INPUT_OUTPUT_DISABLED
    OpenXJ380KeyboardInterruptInfo event = {};
    event.source = OPENXJ380_INPUT_SOURCE_USB;
    event.route = OPENXJ380_INPUT_ROUTE_XJ380_USB;
    event.usb_usage = usage;
    event.value = value;
    event.message_type = (uint8_t)(pressed ? MSG_KEYDOWN : MSG_KEYUP);
    event.pressed = pressed ? 1 : 0;
    OpenXJ380Socket_KeyboardInterrupt(&event);
#else
    keyboard_prepare_fifo();
    bool key_pressed = pressed != 0;
    if (value == KEY_SHIFT)
    {
        kb_fifo.shift = key_pressed;
    }
    else if (value == KEY_CTRL)
    {
        kb_fifo.ctrl = key_pressed;
    }
    else if (value == KEY_ALT)
    {
        kb_fifo.alt = key_pressed;
    }
    else if (usage == 0xe3 || usage == 0xe7)
    {
        kb_fifo.win = key_pressed;
    }
    else if (value == KEY_CAPS && key_pressed)
    {
        kb_fifo.caps = !kb_fifo.caps;
    }

    uint8_t event_value = value;
    if (!key_pressed && event_value == 0)
    {
        event_value = kb_usb_pressed_values[usage];
    }

    if (key_pressed)
    {
        kb_usb_pressed_values[usage] = event_value;
    }
    else
    {
        kb_usb_pressed_values[usage] = 0;
    }

    keyboard_dispatch_key_message(key_pressed ? MSG_KEYDOWN : MSG_KEYUP, event_value);

    OpenXJ380KeyboardInterruptInfo event = {};
    event.source = OPENXJ380_INPUT_SOURCE_USB;
    event.usb_usage = usage;
    event.value = event_value;
    event.message_type = (uint8_t)(key_pressed ? MSG_KEYDOWN : MSG_KEYUP);
    event.pressed = key_pressed ? 1 : 0;
    event.shift = kb_fifo.shift ? 1 : 0;
    event.ctrl = kb_fifo.ctrl ? 1 : 0;
    event.alt = kb_fifo.alt ? 1 : 0;
    event.win = kb_fifo.win ? 1 : 0;
    event.caps = kb_fifo.caps ? 1 : 0;
    OpenXJ380Socket_KeyboardInterrupt(&event);

    kb_synth_lock_acquire();
    keyboard_usb_repeat_slot *free_slot = NULL;
    keyboard_usb_repeat_slot *match = NULL;

    for (size_t i = 0; i < KB_USB_REPEAT_SLOTS; ++i)
    {
        keyboard_usb_repeat_slot *slot = &kb_usb_repeat[i];
        if (slot->active && slot->usage == usage) { match = slot; }
        else if (!slot->active && free_slot == NULL) { free_slot = slot; }
    }

    if (key_pressed)
    {
        kb_synth_enqueue_locked(event_value);
        if (kb_value_repeatable(event_value))
        {
            keyboard_usb_repeat_slot *slot = match ? match : free_slot;
            if (slot != NULL)
            {
                uint64_t now = nanoTime();
                slot->active = true;
                slot->usage = usage;
                slot->value = event_value;
                slot->next_repeat_ns = now ? now + KB_USB_REPEAT_DELAY_NS : 0;
            }
        }
    }
    else
    {
        if (match != NULL) {
            match->active = false;
        }
        if (event_value == KEY_CTRL)
        {
            // Keep legacy terminal input behavior consistent with the PS/2 path.
            kb_synth_enqueue_locked(0x9d);
        }
    }

    kb_synth_lock_release();
#endif
}

EXPORT_SYMBOL(keyboard_push_input);
EXPORT_SYMBOL(keyboard_usb_key_event);
