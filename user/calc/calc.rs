#![no_std]
#![allow(static_mut_refs)]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

#[path = "../rust_i18n.rs"]
mod rust_i18n;

type Hdl = u64;
type MsgProc = extern "C" fn(u64, u64, u64);

const COLOR_BACKGROUND: u32 = 0xF5F7FAFF;
const COLOR_DISPLAY_BG: u32 = 0xFFFFFFFF;
const COLOR_DISPLAY_BORDER: u32 = 0xD1D9E6FF;
const COLOR_NUMBER_BG: u32 = 0xE3E8F0FF;
const COLOR_OPERATOR_BG: u32 = 0xA0AEC0FF;
const COLOR_EQUAL_BG: u32 = 0x4C6EF5FF;
const COLOR_FUNCTION_BG: u32 = 0xF0B429FF;
const COLOR_TEXT_DARK: u32 = 0x2D3748FF;
const COLOR_TEXT_LIGHT: u32 = 0xFFFFFFFF;

const MSG_CHAR: u64 = 0;
const MSG_LBUTTON: u64 = 2;
const MSG_SPCHAR: u64 = 7;
const MSG_RESIZE: u64 = 8;

const XWIN_NORMAL: u8 = 0;
const XWIN_SUPPORT_RESIZEABLE: u8 = 0x80;

#[repr(C)]
struct XWindow {
    width: u32,
    height: u32,
    title: *mut c_char,
    sets: u8,
}

unsafe extern "C" {
    fn xapi_CreateWindow(handle: *mut Hdl, window: *mut XWindow);
    fn xapi_SetWindowTitle(handle: Hdl, title: *mut c_char);
    fn xapi_SetIcon(handle: Hdl, path: *mut c_char);
    fn xapi_DrawRect(handle: Hdl, x1: u32, y1: u32, x2: u32, y2: u32, color: u32, fill: bool);
    fn xapi_DrawText(handle: Hdl, x: u32, y: u32, text: *mut c_char, size: u32, color: u32);
    fn xapi_RefreshWindow(handle: Hdl);
    fn SetMsgPrcor(handle: Hdl, func: MsgProc);
    fn xapi_Sleep(ms: u64);
}

struct CalculatorState {
    display: [u8; 32],
    operand1: i64,
    operand2: i64,
    result: i64,
    operation: u8,
    is_new_operand: bool,
}

impl CalculatorState {
    const fn new() -> Self {
        Self {
            display: [0; 32],
            operand1: 0,
            operand2: 0,
            result: 0,
            operation: 0,
            is_new_operand: true,
        }
    }

    fn reset(&mut self) {
        self.display = [0; 32];
        self.display[0] = b'0';
        self.operand1 = 0;
        self.operand2 = 0;
        self.result = 0;
        self.operation = 0;
        self.is_new_operand = true;
    }

    fn display_len(&self) -> usize {
        cstr_len(&self.display)
    }

    fn display_ptr(&mut self) -> *mut c_char {
        self.display.as_mut_ptr() as *mut c_char
    }

    fn set_display_bytes(&mut self, text: &[u8]) {
        self.display = [0; 32];
        let mut i = 0;
        while i < text.len() && i < self.display.len() - 1 {
            self.display[i] = text[i];
            i += 1;
        }
    }

    fn set_display_int(&mut self, value: i64) {
        let mut buf = [0u8; 32];
        write_i64(value, &mut buf);
        self.set_display_bytes(cstr_slice(&buf));
    }

    fn display_int(&self) -> i64 {
        parse_i64(&self.display)
    }

    fn handle_number(&mut self, num: u8) {
        if self.is_new_operand {
            self.set_display_bytes(b"0");
            self.is_new_operand = false;
        }

        if self.display_len() >= 8 {
            return;
        }

        if self.display[0] == b'0' && self.display[1] == 0 {
            self.display[0] = b'0' + num;
            self.display[1] = 0;
            return;
        }

        let len = self.display_len();
        if len + 1 < self.display.len() {
            self.display[len] = b'0' + num;
            self.display[len + 1] = 0;
        }
    }

    fn perform_calculation(&mut self) {
        self.operand2 = self.display_int();
        match self.operation {
            b'+' => self.result = self.operand1 + self.operand2,
            b'-' => self.result = self.operand1 - self.operand2,
            b'*' => self.result = self.operand1 * self.operand2,
            b'/' => {
                if self.operand2 == 0 {
                    self.set_display_bytes(tr_bytes("错误".as_bytes(), b"Error"));
                    self.is_new_operand = true;
                    return;
                }
                self.result = self.operand1 / self.operand2;
            }
            _ => self.result = self.operand2,
        }

        self.set_display_int(self.result);
        self.is_new_operand = true;
        self.operation = 0;
    }

    fn handle_operator(&mut self, op: u8) {
        if self.operation != 0 && !self.is_new_operand {
            self.perform_calculation();
        }
        self.operand1 = self.display_int();
        self.operation = op;
        self.is_new_operand = true;
    }

    fn handle_backspace(&mut self) {
        if self.is_new_operand || (self.display[0] == b'0' && self.display[1] == 0) {
            return;
        }

        let len = self.display_len();
        if len <= 1 {
            self.set_display_bytes(b"0");
            self.is_new_operand = true;
        } else {
            self.display[len - 1] = 0;
        }
    }

    fn handle_negate(&mut self) {
        if self.display[0] == b'0' && self.display[1] == 0 {
            return;
        }
        let value = -self.display_int();
        self.set_display_int(value);
        self.is_new_operand = true;
    }
}

static mut STATE: CalculatorState = CalculatorState::new();
static mut WINDOW_HANDLE: Hdl = 0;
static mut CALC_WINDOW_WIDTH: i32 = 380;
static mut CALC_WINDOW_HEIGHT: i32 = 480;
static mut LANGUAGE: c_int = rust_i18n::XJ380_LANGUAGE_ZH_CN;

fn cstr_len(buf: &[u8]) -> usize {
    let mut len = 0;
    while len < buf.len() && buf[len] != 0 {
        len += 1;
    }
    len
}

fn cstr_slice(buf: &[u8]) -> &[u8] {
    &buf[..cstr_len(buf)]
}

fn current_language() -> c_int {
    unsafe { LANGUAGE }
}

fn tr_bytes(zh_cn: &'static [u8], en_us: &'static [u8]) -> &'static [u8] {
    rust_i18n::tr_bytes_lang(current_language(), zh_cn, en_us)
}

fn tr_cstr(zh_cn: *const c_char, en_us: *const c_char) -> *mut c_char {
    rust_i18n::tr_cstr_lang(current_language(), zh_cn, en_us)
}

fn parse_i64(buf: &[u8]) -> i64 {
    let mut idx = 0;
    let mut negative = false;
    if idx < buf.len() && buf[idx] == b'-' {
        negative = true;
        idx += 1;
    } else if idx < buf.len() && buf[idx] == b'+' {
        idx += 1;
    }

    let mut value = 0i64;
    while idx < buf.len() {
        let ch = buf[idx];
        if !(b'0'..=b'9').contains(&ch) {
            break;
        }
        value = value * 10 + i64::from(ch - b'0');
        idx += 1;
    }

    if negative { -value } else { value }
}

fn write_i64(value: i64, out: &mut [u8; 32]) {
    out.fill(0);
    if value == 0 {
        out[0] = b'0';
        return;
    }

    let negative = value < 0;
    let mut n = if negative { value.wrapping_neg() as u64 } else { value as u64 };
    let mut tmp = [0u8; 32];
    let mut len = 0usize;
    while n > 0 && len < tmp.len() {
        tmp[len] = b'0' + (n % 10) as u8;
        n /= 10;
        len += 1;
    }
    if negative && len < tmp.len() {
        tmp[len] = b'-';
        len += 1;
    }

    let mut i = 0usize;
    while i < len && i < out.len() - 1 {
        out[i] = tmp[len - 1 - i];
        i += 1;
    }
}

fn calc_origin_x() -> i32 {
    let width = unsafe { CALC_WINDOW_WIDTH };
    if width > 380 { (width - 380) / 2 } else { 0 }
}

fn calc_origin_y() -> i32 {
    let height = unsafe { CALC_WINDOW_HEIGHT };
    if height > 480 { (height - 480) / 2 } else { 0 }
}

fn draw_text(handle: Hdl, x: i32, y: i32, text: *mut c_char, size: u32, color: u32) {
    unsafe {
        xapi_DrawText(handle, x.max(0) as u32, y.max(0) as u32, text, size, color);
    }
}

fn draw_rect(handle: Hdl, x1: i32, y1: i32, x2: i32, y2: i32, color: u32, fill: bool) {
    unsafe {
        xapi_DrawRect(
            handle,
            x1.max(0) as u32,
            y1.max(0) as u32,
            x2.max(0) as u32,
            y2.max(0) as u32,
            color,
            fill,
        );
    }
}

fn update_display(handle: Hdl) {
    let ox = calc_origin_x();
    let oy = calc_origin_y();
    draw_rect(handle, ox + 20, oy + 20, ox + 340, oy + 90, COLOR_DISPLAY_BG, true);
    draw_rect(handle, ox + 20, oy + 20, ox + 340, oy + 90, COLOR_DISPLAY_BORDER, false);

    unsafe {
        let text_len = STATE.display_len() as i32;
        let mut text_x = ox + 340 - text_len * 18;
        if text_x < ox + 30 {
            text_x = ox + 30;
        }
        draw_text(handle, text_x, oy + 20 + (70 - 24) / 2 - 10, STATE.display_ptr(), 24, COLOR_TEXT_DARK);
    }
}

fn draw_button(handle: Hdl, x: i32, y: i32, text: *mut c_char, bg_color: u32, text_color: u32) {
    draw_button_sized(handle, x, y, 75, 60, text, bg_color, text_color);
}

fn draw_wide_button(handle: Hdl, x: i32, y: i32, text: *mut c_char, bg_color: u32, text_color: u32) {
    draw_button_sized(handle, x, y, 160, 60, text, bg_color, text_color);
}

fn draw_button_sized(handle: Hdl, x: i32, y: i32, width: i32, height: i32, text: *mut c_char, bg_color: u32, text_color: u32) {
    let x = x + calc_origin_x();
    let y = y + calc_origin_y();
    draw_rect(handle, x, y, x + width, y + height, bg_color, true);
    draw_rect(handle, x, y, x + width, y + height, COLOR_DISPLAY_BORDER, false);

    let text_len = unsafe { c_char_len(text) as i32 };
    let mut text_x = x + (width - text_len * 12) / 2;
    if text_x < x + 5 {
        text_x = x + 5;
    }
    draw_text(handle, text_x, y + (height - 20) / 2 - 10, text, 20, text_color);
}

unsafe fn c_char_len(text: *const c_char) -> usize {
    let mut len = 0usize;
    while unsafe { *text.add(len) } != 0 {
        len += 1;
    }
    len
}

fn draw_calculator_interface(handle: Hdl) {
    let ox = calc_origin_x();
    let oy = calc_origin_y();
    let width = unsafe { CALC_WINDOW_WIDTH };
    let height = unsafe { CALC_WINDOW_HEIGHT };
    draw_rect(handle, 0, 0, width - 1, height - 1, COLOR_BACKGROUND, true);

    draw_text(
        handle,
        ox + 120,
        oy + 10,
        tr_cstr(c"计算器".as_ptr(), c"Calculator".as_ptr()),
        20,
        COLOR_TEXT_DARK,
    );
    update_display(handle);

    draw_button(handle, 20, 110, c"C".as_ptr() as *mut c_char, COLOR_FUNCTION_BG, COLOR_TEXT_LIGHT);
    draw_button(handle, 105, 110, c"←".as_ptr() as *mut c_char, COLOR_FUNCTION_BG, COLOR_TEXT_LIGHT);
    draw_button(handle, 190, 110, c"±".as_ptr() as *mut c_char, COLOR_FUNCTION_BG, COLOR_TEXT_LIGHT);
    draw_button(handle, 275, 110, c"÷".as_ptr() as *mut c_char, COLOR_OPERATOR_BG, COLOR_TEXT_LIGHT);

    draw_button(handle, 20, 180, c"7".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 105, 180, c"8".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 190, 180, c"9".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 275, 180, c"×".as_ptr() as *mut c_char, COLOR_OPERATOR_BG, COLOR_TEXT_LIGHT);

    draw_button(handle, 20, 250, c"4".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 105, 250, c"5".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 190, 250, c"6".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 275, 250, c"-".as_ptr() as *mut c_char, COLOR_OPERATOR_BG, COLOR_TEXT_LIGHT);

    draw_button(handle, 20, 320, c"1".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 105, 320, c"2".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 190, 320, c"3".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 275, 320, c"+".as_ptr() as *mut c_char, COLOR_OPERATOR_BG, COLOR_TEXT_LIGHT);

    draw_wide_button(handle, 20, 390, c"0".as_ptr() as *mut c_char, COLOR_NUMBER_BG, COLOR_TEXT_DARK);
    draw_button(handle, 190, 390, c"=".as_ptr() as *mut c_char, COLOR_EQUAL_BG, COLOR_TEXT_LIGHT);
}

fn button_id_at(mut x: i32, mut y: i32) -> u64 {
    x -= calc_origin_x();
    y -= calc_origin_y();

    for &(id, bx, by, bw, bh) in BUTTONS {
        if x >= bx && x <= bx + bw && y >= by && y <= by + bh {
            return id;
        }
    }
    0
}

const BUTTONS: &[(u64, i32, i32, i32, i32)] = &[
    (300, 20, 110, 75, 60),
    (301, 105, 110, 75, 60),
    (302, 190, 110, 75, 60),
    (203, 275, 110, 75, 60),
    (107, 20, 180, 75, 60),
    (108, 105, 180, 75, 60),
    (109, 190, 180, 75, 60),
    (202, 275, 180, 75, 60),
    (104, 20, 250, 75, 60),
    (105, 105, 250, 75, 60),
    (106, 190, 250, 75, 60),
    (201, 275, 250, 75, 60),
    (101, 20, 320, 75, 60),
    (102, 105, 320, 75, 60),
    (103, 190, 320, 75, 60),
    (200, 275, 320, 75, 60),
    (100, 20, 390, 160, 60),
    (204, 190, 390, 75, 60),
];

fn process_button(button_id: u64) {
    unsafe {
        if (100..=109).contains(&button_id) {
            STATE.handle_number((button_id - 100) as u8);
        } else {
            match button_id {
                200 => STATE.handle_operator(b'+'),
                201 => STATE.handle_operator(b'-'),
                202 => STATE.handle_operator(b'*'),
                203 => STATE.handle_operator(b'/'),
                204 => {
                    if STATE.operation != 0 {
                        STATE.perform_calculation();
                    }
                }
                300 => STATE.reset(),
                301 => STATE.handle_backspace(),
                302 => STATE.handle_negate(),
                _ => return,
            }
        }
        update_display(WINDOW_HANDLE);
        xapi_RefreshWindow(WINDOW_HANDLE);
    }
}

extern "C" fn message_handler(msg_type: u64, h_data: u64, l_data: u64) {
    unsafe {
        match msg_type {
            MSG_LBUTTON => process_button(button_id_at(h_data as i32, l_data as i32)),
            MSG_CHAR => {
                if (b'0' as u64..=b'9' as u64).contains(&l_data) {
                    STATE.handle_number((l_data as u8) - b'0');
                } else {
                    match l_data as u8 {
                        b'+' | b'-' | b'*' | b'/' => STATE.handle_operator(l_data as u8),
                        b'=' => {
                            if STATE.operation != 0 {
                                STATE.perform_calculation();
                            }
                        }
                        _ => return,
                    }
                }
                update_display(WINDOW_HANDLE);
                xapi_RefreshWindow(WINDOW_HANDLE);
            }
            MSG_SPCHAR => {
                if l_data == b'\x08' as u64 {
                    STATE.handle_backspace();
                } else if l_data == b'\n' as u64 {
                    if STATE.operation != 0 {
                        STATE.perform_calculation();
                    }
                } else {
                    return;
                }
                update_display(WINDOW_HANDLE);
                xapi_RefreshWindow(WINDOW_HANDLE);
            }
            MSG_RESIZE => {
                CALC_WINDOW_WIDTH = h_data as i32;
                CALC_WINDOW_HEIGHT = l_data as i32;
                draw_calculator_interface(WINDOW_HANDLE);
                xapi_RefreshWindow(WINDOW_HANDLE);
            }
            _ => {}
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn main(_argc: c_int, _argv: *mut *mut c_char, _envp: *mut *mut c_char) -> c_int {
    unsafe {
        STATE.reset();
        LANGUAGE = rust_i18n::read_language();

        let mut window = XWindow {
            width: 380,
            height: 480,
            title: tr_cstr(c"计算器".as_ptr(), c"Calculator".as_ptr()),
            sets: XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE,
        };

        xapi_CreateWindow(&raw mut WINDOW_HANDLE, &mut window);
        xapi_SetIcon(WINDOW_HANDLE, c"/system/icon/calc.png".as_ptr() as *mut c_char);
        SetMsgPrcor(WINDOW_HANDLE, message_handler);
        draw_calculator_interface(WINDOW_HANDLE);
        xapi_RefreshWindow(WINDOW_HANDLE);

        loop {
            xapi_Sleep(1);
        }
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
