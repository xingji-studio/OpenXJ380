#![no_std]
#![allow(static_mut_refs)]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

#[path = "../rust_i18n.rs"]
mod rust_i18n;

type Hdl = u64;
type MsgProc = extern "C" fn(u64, u64, u64);

const WINDOW_WIDTH: u32 = 520;
const WINDOW_HEIGHT: u32 = 360;

const MSG_CRL: u64 = 6;
const XWIN_NORMAL: u8 = 0;

const COLOR_BG: u32 = 0xf5f9ffff;
const COLOR_PANEL: u32 = 0xffffffff;
const COLOR_BLUE: u32 = 0x0078d4ff;
const COLOR_BLUE_DARK: u32 = 0x005a9eff;
const COLOR_TEXT: u32 = 0x1f2937ff;
const COLOR_MUTED: u32 = 0x526172ff;
const COLOR_LINE: u32 = 0xd7e3f0ff;
const COLOR_LOGO_BG: u32 = 0xe7f1fbff;
const BUTTON_OK: u64 = 1;

#[repr(C)]
struct XWindow {
    width: u32,
    height: u32,
    title: *mut c_char,
    sets: u8,
}

unsafe extern "C" {
    fn xapi_CreateWindow(handle: *mut Hdl, window: *mut XWindow);
    fn xapi_CloseWindow(handle: Hdl);
    fn xapi_SetWindowTitle(handle: Hdl, title: *mut c_char);
    fn xapi_SetIcon(handle: Hdl, path: *mut c_char);
    fn xapi_DrawRect(handle: Hdl, x1: u32, y1: u32, x2: u32, y2: u32, color: u32, fill: bool);
    fn xapi_DrawText(handle: Hdl, x: u32, y: u32, text: *mut c_char, size: u32, color: u32);
    fn xapi_DrawPicture(handle: Hdl, x: u32, y: u32, width: u32, height: u32, path: *mut c_char);
    fn xapi_Button(handle: Hdl, id: u64, x: u64, y: u64, text: *mut c_char);
    fn xapi_GetSystemVersion(version: *mut c_char);
    fn xapi_RefreshWindow(handle: Hdl);
    fn SetMsgPrcor(handle: Hdl, func: MsgProc);
    fn xapi_Sleep(ms: u64);
    fn xapi_Exit(value: u64);
}

static mut WINDOW_HANDLE: Hdl = 0;
static mut EXIT_REQUESTED: bool = false;
static mut LANGUAGE: c_int = rust_i18n::XJ380_LANGUAGE_ZH_CN;

static KERNEL_VERSION: &[u8] = include_bytes!("../../kernel/build_settings.h");

fn current_language() -> c_int {
    unsafe { LANGUAGE }
}

fn tr_cstr(zh_cn: *const c_char, en_us: *const c_char) -> *mut c_char {
    rust_i18n::tr_cstr_lang(current_language(), zh_cn, en_us)
}

fn draw_header(handle: Hdl) {
    unsafe {
        xapi_DrawRect(handle, 0, 0, WINDOW_WIDTH - 1, 72, COLOR_BLUE, true);
        xapi_DrawRect(handle, 0, 68, WINDOW_WIDTH - 1, 72, COLOR_BLUE_DARK, true);
        xapi_DrawText(handle, 24, 18, tr_cstr(c"关于 XJ380".as_ptr(), c"About XJ380".as_ptr()), 24, 0xffffffff);
        xapi_DrawText(
            handle,
            26,
            50,
            tr_cstr(c"XINGJI 桌面系统".as_ptr(), c"XINGJI Desktop System".as_ptr()),
            11,
            0xeaf6ffff,
        );
    }
}

fn draw_logo(handle: Hdl) {
    unsafe {
        xapi_DrawRect(handle, 30, 100, 137, 207, COLOR_LOGO_BG, true);
        xapi_DrawRect(handle, 30, 100, 137, 103, COLOR_BLUE, true);
        xapi_DrawRect(handle, 30, 204, 137, 207, COLOR_BLUE, true);
        xapi_DrawPicture(handle, 46, 124, 76, 58, c"/system/xj380.png".as_ptr() as *mut c_char);
    }
}

fn copy_system_version(buf: &mut [u8; 96]) {
    unsafe {
        xapi_GetSystemVersion(buf.as_mut_ptr() as *mut c_char);
    }
    buf[buf.len() - 1] = 0;
}

fn copy_kernel_version(buf: &mut [u8; 96]) {
    const KEY: &[u8] = b"#define CONFIG_KN_VERSION \"";
    let mut i = 0;
    while i + KEY.len() < KERNEL_VERSION.len() {
        let mut matched = true;
        let mut j = 0;
        while j < KEY.len() {
            if KERNEL_VERSION[i + j] != KEY[j] {
                matched = false;
                break;
            }
            j += 1;
        }

        if matched {
            let mut out = 0;
            let mut src = i + KEY.len();
            while src < KERNEL_VERSION.len() && KERNEL_VERSION[src] != b'"' && out + 1 < buf.len() {
                buf[out] = KERNEL_VERSION[src];
                out += 1;
                src += 1;
            }
            buf[out] = 0;
            return;
        }
        i += 1;
    }

    copy_bytes(buf, b"XSK");
}

fn copy_bytes(buf: &mut [u8], text: &[u8]) {
    if buf.is_empty() {
        return;
    }

    let mut i = 0;
    while i + 1 < buf.len() && i < text.len() {
        buf[i] = text[i];
        i += 1;
    }
    buf[i] = 0;
}

fn draw_info(handle: Hdl) {
    let mut system_version = [0u8; 96];
    let mut kernel_version = [0u8; 96];
    copy_system_version(&mut system_version);
    copy_kernel_version(&mut kernel_version);

    unsafe {
        xapi_DrawText(
            handle,
            168,
            98,
            tr_cstr(c"XJ380 操作系统".as_ptr(), c"XJ380 Operating System".as_ptr()),
            22,
            COLOR_TEXT,
        );
        xapi_DrawText(handle, 170, 134, system_version.as_mut_ptr() as *mut c_char, 12, COLOR_MUTED);

        xapi_DrawRect(handle, 168, 172, 476, 173, COLOR_LINE, true);
        xapi_DrawText(handle, 170, 194, tr_cstr(c"内核版本".as_ptr(), c"Kernel".as_ptr()), 11, COLOR_MUTED);
        xapi_DrawText(handle, 252, 194, kernel_version.as_mut_ptr() as *mut c_char, 11, COLOR_TEXT);
        xapi_DrawText(handle, 170, 222, tr_cstr(c"系统类型".as_ptr(), c"System Type".as_ptr()), 11, COLOR_MUTED);
        xapi_DrawText(
            handle,
            252,
            222,
            tr_cstr(c"x64 操作系统，基于 x86_64 处理器".as_ptr(), c"x64 OS, x86_64 processor".as_ptr()),
            11,
            COLOR_TEXT,
        );

        xapi_DrawText(
            handle,
            30,
            250,
            c"Copyright (C) XINGJI Interactive Software 2017 - 2026".as_ptr() as *mut c_char,
            10,
            COLOR_MUTED,
        );
        xapi_DrawText(
            handle,
            30,
            272,
            tr_cstr(
                c"本产品按原样提供，适用于 XJ380 桌面环境。".as_ptr(),
                c"Provided as-is for the XJ380 desktop environment.".as_ptr(),
            ),
            10,
            COLOR_MUTED,
        );

        xapi_Button(handle, BUTTON_OK, 426, 312, tr_cstr(c"确定".as_ptr(), c"OK".as_ptr()));
    }
}

fn draw_window(handle: Hdl) {
    unsafe {
        xapi_DrawRect(handle, 0, 0, WINDOW_WIDTH - 1, WINDOW_HEIGHT - 1, COLOR_BG, true);
        xapi_DrawRect(handle, 18, 88, WINDOW_WIDTH - 19, 296, COLOR_PANEL, true);
    }
    draw_header(handle);
    draw_logo(handle);
    draw_info(handle);
    unsafe {
        xapi_RefreshWindow(handle);
    }
}

extern "C" fn message_handler(msg_type: u64, h_data: u64, _l_data: u64) {
    unsafe {
        if msg_type == MSG_CRL && h_data == BUTTON_OK {
            EXIT_REQUESTED = true;
            xapi_CloseWindow(WINDOW_HANDLE);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn main(_argc: c_int, _argv: *mut *mut c_char, _envp: *mut *mut c_char) -> c_int {
    unsafe {
        LANGUAGE = rust_i18n::read_language();
        let mut window = XWindow {
            width: WINDOW_WIDTH,
            height: WINDOW_HEIGHT,
            title: tr_cstr(c"关于 XJ380".as_ptr(), c"About XJ380".as_ptr()),
            sets: XWIN_NORMAL,
        };

        xapi_CreateWindow(&raw mut WINDOW_HANDLE, &mut window);
        xapi_SetIcon(WINDOW_HANDLE, c"/system/icon/xjver.png".as_ptr() as *mut c_char);
        SetMsgPrcor(WINDOW_HANDLE, message_handler);
        draw_window(WINDOW_HANDLE);

        loop {
            if EXIT_REQUESTED {
                xapi_Exit(0);
            }
            xapi_Sleep(50);
        }
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
