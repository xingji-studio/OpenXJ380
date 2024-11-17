#include "include.h"

PRIVATE int code_with_E0 = 0;
PRIVATE int shift_l;
PRIVATE int shift_r;
PRIVATE int alt_l;
PRIVATE int alt_r;
PRIVATE int ctrl_l;
PRIVATE int ctrl_r;
PRIVATE int caps_lock;
PRIVATE int num_lock;
PRIVATE int scroll_lock;
PRIVATE int column;

PRIVATE void keyboard_read();

PUBLIC void keyboard_handler(int irq)   //将按键放入FIFO缓冲区并读取
{
    fifo_put(&keyfifo, in_byte(KB_DATA));
    keyboard_read();
}

PUBLIC void kb_wait()       //等待
{
    u8 kb_stat;

    do {
        kb_stat = in_byte(KB_CMD);
    } while (kb_stat & 0x02);
}

PRIVATE void kb_ack()       //意义不明
{
    u8 kb_read;

    do {
        kb_read = in_byte(KB_DATA);
    } while (kb_read != KB_ACK);
}

PRIVATE void set_leds()     //意义不明
{
    u8 leds = (caps_lock << 2) | (num_lock << 1) | scroll_lock;

    kb_wait();
    out_byte(KB_DATA, LED_CODE);
    kb_ack();

    kb_wait();
    out_byte(KB_DATA, leds);
    kb_ack();
}


PUBLIC void init_keyboard() //初始化键盘中断
{
    fifo_init(&keyfifo, 32, keybuf);
    fifo_init(&decoded_key, 32, dkey_buf);

    shift_l = shift_r = 0;
    alt_l   = alt_r   = 0;
    ctrl_l  = ctrl_r  = 0;

    caps_lock   = 0;
    num_lock    = 1;
    scroll_lock = 0;

    set_leds();

    put_irq_handler(KEYBOARD_IRQ, keyboard_handler);
    enable_irq(KEYBOARD_IRQ);
}

PRIVATE u8 get_scancode()   //意义不明
{
    u8 scan_code;
    disable_int();
    scan_code = fifo_get(&keyfifo);
    enable_int();
    return scan_code;
}

PRIVATE void in_process(u32 key)    //识别换行符和回车符
{
    char output[2] = {0, 0};

    if (!(key & FLAG_EXT)) {
        fifo_put(&decoded_key, key);
    } else {
        int raw_code = key & MASK_RAW;
        switch (raw_code) {
            case ENTER:
                fifo_put(&decoded_key, '\n');
                break;
            case BACKSPACE:
                fifo_put(&decoded_key, '\b');
                break;
        }
    }
}

PRIVATE void keyboard_read()    //识别按键
{
    u8 scan_code;
    int make;

    u32 key = 0;
    u32 *keyrow;

    if (fifo_status(&keyfifo) > 0) {
        code_with_E0 = 0;

        scan_code = get_scancode();
        
        if (scan_code == 0xE1) {
            int i;
            u8 pausebrk_scode[] = {0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5};
            int is_pausebreak = 1;
            for (i = 1; i <= 6; i++) {
                if (get_scancode() != pausebrk_scode[i]) {
                    is_pausebreak = 0;
                    break;
                }
            }

            if (is_pausebreak) key = PAUSEBREAK;
        } else if (scan_code == 0xE0) {
            scan_code = get_scancode();
            if (scan_code == 0x2A) {
                if (get_scancode() == 0xE0) {
                    if (get_scancode() == 0x37) {
                        key = PRINTSCREEN;
                        make = 1;
                    }
                }
            }

            if (scan_code == 0xB7) {
                if (get_scancode() == 0xE0) {
                    if (get_scancode() == 0xAA) {
                        key = PRINTSCREEN;
                        make = 0;
                    }
                }
            }

            if (key == 0) {
                code_with_E0 = 1;
            }
        }
        if ((key != PAUSEBREAK) && (key != PRINTSCREEN)) {
            make = (scan_code & FLAG_BREAK ? false : true);

            keyrow = &keymap[(scan_code & 0x7F) * MAP_COLS];

            column = 0;

            int caps = shift_l || shift_r;
            if (caps_lock) {
                if ((keyrow[0] >= 'a') && (keyrow[0] <= 'z')) {
                    caps = !caps;
                }
            }

            if (caps) {
                column = 1;
            }
            if (code_with_E0) {
                column = 2;
            }

            key = keyrow[column];

            switch (key) {
                case SHIFT_L:
                    shift_l = make;
                    break;
                case SHIFT_R:
                    shift_r = make;
                    break;
                case CTRL_L:
                    ctrl_l = make;
                    break;
                case CTRL_R:
                    ctrl_r = make;
                    break;
                case ALT_L:
                    alt_l = make;
                    break;
                case ALT_R:
                    alt_r = make;
                    break;
                case CAPS_LOCK:
                    if (make) {
                        caps_lock = !caps_lock;
                        set_leds();
                    }
                    break;
                case NUM_LOCK:
                    if (make) {
                        num_lock = !num_lock;
                        set_leds();
                    }
                    break;
                case SCROLL_LOCK:
                    if (make) {
                        scroll_lock = !scroll_lock;
                        set_leds();
                    }
                    break;
                default:
                    break;
            }

            if (make) {
                int pad = 0;

                if ((key >= PAD_SLASH) && (key <= PAD_9)) {
                    pad = 1;
                    switch (key) {
                        case PAD_SLASH:
                            key = '/';
                            break;
                        case PAD_STAR:
                            key = '*';
                            break;
                        case PAD_MINUS:
                            key = '-';
                            break;
                        case PAD_PLUS:
                            key = '+';
                            break;
                        case PAD_ENTER:
                            key = ENTER;
                            break;
                        default:
                            if (num_lock && (key >= PAD_0) && (key <= PAD_9)) {
                                key = key - PAD_0 + '0';
                            } else if (num_lock && (key == PAD_DOT)) {
                                key = '.';
                            } else {
                                switch (key) {
                                    case PAD_HOME:
                                        key = HOME;
                                        break;
                                    case PAD_END:
                                        key = END;
                                        break;
                                    case PAD_PAGEUP:
                                        key = PAGEUP;
                                        break;
                                    case PAD_PAGEDOWN:
                                        key = PAD_PAGEDOWN;
                                        break;
                                    case PAD_INS:
                                        key = INSERT;
                                        break;
                                    case PAD_UP:
                                        key = UP;
                                        break;
                                    case PAD_DOWN:
                                        key = DOWN;
                                        break;
                                    case PAD_LEFT:
                                        key = LEFT;
                                        break;
                                    case PAD_RIGHT:
                                        key = RIGHT;
                                        break;
                                    case PAD_DOT:
                                        key = DELETE;
                                        break;
                                    default:
                                        break;
                                }
                            }
                            break;
                    }
                }
                key |= shift_l ? FLAG_SHIFT_L : 0;
                key |= shift_r ? FLAG_SHIFT_R : 0;
                key |= ctrl_l  ? FLAG_CTRL_L  : 0;
                key |= ctrl_r  ? FLAG_CTRL_R  : 0;
                key |= alt_l   ? FLAG_ALT_L   : 0;
                key |= alt_r   ? FLAG_ALT_R   : 0;
                key |= pad     ? FLAG_PAD     : 0;

                in_process(key);
            }
        }
    }
}