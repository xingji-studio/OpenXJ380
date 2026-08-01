#![no_std]
#![allow(static_mut_refs)]

use core::ffi::{c_char, c_int, c_long, c_ulong, c_void};
use core::panic::PanicInfo;
use core::ptr;

#[path = "rust_i18n.rs"]
mod rust_i18n;

type SizeT = usize;
type SSizeT = isize;
type Hdl = u64;
type MsgProc = extern "C" fn(u64, u64, u64);

const BUSY_WIDTH: i32 = 720;
const BUSY_HEIGHT: i32 = 405;
const TERM_LEFT_X: i32 = 8;
const TERM_TOP_Y: i32 = 4;
const TERM_RIGHT_PAD: i32 = 8;
const TERM_BOTTOM_PAD: i32 = 16;
const CELL_W: i32 = 9;
const CELL_H: i32 = 16;
const COLOR_BG: u32 = 0x0b0f14ff;
const COLOR_FG: u32 = 0xe6edf3ff;
const PTY_CHUNK: usize = 1024;
const DEFAULT_SHELL: *const c_char = c"/bin/sh".as_ptr();
const BUSYBOX_PATH: *const c_char = c"/apps/busybox".as_ptr();
const RESIZE_DEBOUNCE_NS: u64 = 30_000_000;

const MSG_CHAR: u64 = 0;
const MSG_SPCHAR: u64 = 7;
const MSG_RESIZE: u64 = 8;

const XWIN_NORMAL: u8 = 0;
const XWIN_SUPPORT_RESIZEABLE: u8 = 0x80;

const O_RDWR: c_int = 2;
const O_NOCTTY: c_int = 0o400;
const TIOCSCTTY: c_ulong = 0x540E;
const TIOCSWINSZ: c_ulong = 0x5414;
const POLLIN: i16 = 0x0001;
const POLLHUP: i16 = 0x0010;
const WNOHANG: c_int = 1;
const CLOCK_MONOTONIC: c_int = 1;

const TERM_KEY_ESC: u64 = 128;
const TERM_KEY_TAB: u64 = 130;
const TERM_KEY_SHIFT: u64 = 133;
const TERM_KEY_CTRL: u64 = 134;
const TERM_KEY_ALT: u64 = 135;
const TERM_KEY_F1: u64 = 136;
const TERM_KEY_F2: u64 = 137;
const TERM_KEY_F3: u64 = 138;
const TERM_KEY_F4: u64 = 139;
const TERM_KEY_F5: u64 = 140;
const TERM_KEY_F6: u64 = 141;
const TERM_KEY_F7: u64 = 142;
const TERM_KEY_F8: u64 = 143;
const TERM_KEY_F9: u64 = 144;
const TERM_KEY_F10: u64 = 145;
const TERM_KEY_F11: u64 = 146;
const TERM_KEY_F12: u64 = 147;
const TERM_KEY_HOME: u64 = 150;
const TERM_KEY_UP: u64 = 151;
const TERM_KEY_PAGE_UP: u64 = 152;
const TERM_KEY_LEFT: u64 = 153;
const TERM_KEY_RIGHT: u64 = 154;
const TERM_KEY_END: u64 = 155;
const TERM_KEY_DOWN: u64 = 156;
const TERM_KEY_PAGE_DOWN: u64 = 157;
const TERM_KEY_INSERT: u64 = 158;
const TERM_KEY_DELETE: u64 = 159;
const TERM_KEY_CTRL_RELEASE: u64 = 0x9d;

const VTERM_MAX_CHARS_PER_CELL: usize = 6;
const VTERM_PROP_CURSORVISIBLE: c_int = 1;
const VTERM_DAMAGE_SCROLL: c_int = 3;
const VTERM_COLOR_TYPE_MASK: u8 = 0x01;
const VTERM_COLOR_INDEXED: u8 = 0x01;
const VTERM_COLOR_DEFAULT_FG: u8 = 0x02;
const VTERM_COLOR_DEFAULT_BG: u8 = 0x04;

#[repr(C)]
struct XWindow {
    width: u32,
    height: u32,
    title: *mut c_char,
    sets: u8,
}

#[repr(C)]
struct XColor {
    red: u8,
    green: u8,
    blue: u8,
}

#[repr(C)]
struct UserInfo {
    name: [c_char; 64],
    user_type: c_int,
}

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: c_long,
}

#[repr(C)]
struct Winsize {
    ws_row: u16,
    ws_col: u16,
    ws_xpixel: u16,
    ws_ypixel: u16,
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

#[repr(C)]
struct VTerm {
    _private: [u8; 0],
}

#[repr(C)]
struct VTermScreen {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Copy, Clone)]
struct VTermPos {
    row: c_int,
    col: c_int,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct VTermRect {
    start_row: c_int,
    end_row: c_int,
    start_col: c_int,
    end_col: c_int,
}

#[repr(C)]
#[derive(Copy, Clone)]
union VTermColor {
    type_: u8,
    rgb: VTermColorRgb,
    indexed: VTermColorIndexed,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct VTermColorRgb {
    type_: u8,
    red: u8,
    green: u8,
    blue: u8,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct VTermColorIndexed {
    type_: u8,
    idx: u8,
}

#[repr(C)]
union VTermValue {
    boolean: c_int,
    number: c_int,
    bytes: [u8; 16],
}

#[repr(C)]
struct VTermBuilder {
    ver: c_int,
    rows: c_int,
    cols: c_int,
    allocator: *const c_void,
    allocdata: *mut c_void,
    outbuffer_len: SizeT,
    tmpbuffer_len: SizeT,
}

#[repr(C)]
struct VTermScreenCell {
    chars: [u32; VTERM_MAX_CHARS_PER_CELL],
    width: c_char,
    _pad: [u8; 3],
    attrs: u32,
    fg: VTermColor,
    bg: VTermColor,
}

#[repr(C)]
struct VTermScreenCallbacks {
    damage: Option<extern "C" fn(VTermRect, *mut c_void) -> c_int>,
    moverect: Option<extern "C" fn(VTermRect, VTermRect, *mut c_void) -> c_int>,
    movecursor: Option<extern "C" fn(VTermPos, VTermPos, c_int, *mut c_void) -> c_int>,
    settermprop: Option<extern "C" fn(c_int, *mut VTermValue, *mut c_void) -> c_int>,
    bell: Option<extern "C" fn(*mut c_void) -> c_int>,
    resize: Option<extern "C" fn(c_int, c_int, *mut c_void) -> c_int>,
    sb_pushline: Option<extern "C" fn(c_int, *const VTermScreenCell, *mut c_void) -> c_int>,
    sb_popline: Option<extern "C" fn(c_int, *mut VTermScreenCell, *mut c_void) -> c_int>,
    sb_clear: Option<extern "C" fn(*mut c_void) -> c_int>,
    sb_pushline4: Option<extern "C" fn(c_int, *const VTermScreenCell, bool, *mut c_void) -> c_int>,
}

static VTERM_CALLBACKS: VTermScreenCallbacks = VTermScreenCallbacks {
    damage: Some(vterm_damage),
    moverect: Some(vterm_moverect),
    movecursor: Some(vterm_movecursor),
    settermprop: Some(vterm_settermprop),
    bell: None,
    resize: None,
    sb_pushline: None,
    sb_popline: None,
    sb_clear: None,
    sb_pushline4: None,
};

static mut G_WINDOW: Hdl = 0;
static mut G_CHILD_PID: c_int = -1;
static mut G_PTY_MASTER: c_int = -1;
static mut G_SPAWN_ERROR: c_int = 0;
static mut G_SPAWN_STAGE: *const c_char = c"start".as_ptr();
static mut G_CTRL_DOWN: bool = false;
static mut G_WIN_WIDTH: i32 = BUSY_WIDTH;
static mut G_WIN_HEIGHT: i32 = BUSY_HEIGHT;
static mut G_TERM_COLS: i32 = (BUSY_WIDTH - TERM_LEFT_X - TERM_RIGHT_PAD) / CELL_W;
static mut G_TERM_ROWS: i32 = (BUSY_HEIGHT - TERM_TOP_Y - TERM_BOTTOM_PAD) / CELL_H;
static mut G_VTERM: *mut VTerm = ptr::null_mut();
static mut G_VSCREEN: *mut VTermScreen = ptr::null_mut();
static mut G_CURSOR: VTermPos = VTermPos { row: 0, col: 0 };
static mut G_CURSOR_VISIBLE: bool = true;
static mut G_SCROLL_BUFFER: *mut XColor = ptr::null_mut();
static mut G_SCROLL_BUFFER_PIXELS: usize = 0;
static mut G_RESIZE_PENDING: bool = false;
static mut G_RESIZE_PENDING_WIDTH: i32 = BUSY_WIDTH;
static mut G_RESIZE_PENDING_HEIGHT: i32 = BUSY_HEIGHT;
static mut G_RESIZE_LAST_EVENT_NS: u64 = 0;
static mut G_TERMINAL_PAINTED: bool = false;
static mut G_LANGUAGE: c_int = rust_i18n::XJ380_LANGUAGE_ZH_CN;
static mut ENV_USERNAME: [u8; 64] = [0; 64];
static mut ENV_HOME: [u8; 104] = [0; 104];
static mut ENV_USER: [u8; 80] = [0; 80];
static mut ENV_LOGNAME: [u8; 88] = [0; 88];
static mut ENV_PS1: [u8; 180] = [0; 180];

static PALETTE: [u32; 16] = [
    0x0b0f14ff, 0xff7b72ff, 0x3fb950ff, 0xd29922ff,
    0x58a6ffff, 0xbc8cffff, 0x39c5cfff, 0xb1bac4ff,
    0x6e7681ff, 0xffa198ff, 0x56d364ff, 0xe3b341ff,
    0x79c0ffff, 0xd2a8ffff, 0x56d4ddff, 0xf0f6fcff,
];

unsafe extern "C" {
    static mut errno: c_int;
    fn xapi_CreateWindow(handle: *mut Hdl, window: *mut XWindow);
    fn xapi_SetWindowTitle(handle: Hdl, title: *mut c_char);
    fn xapi_SetIcon(handle: Hdl, path: *mut c_char);
    fn xapi_DrawRect(handle: Hdl, x1: u32, y1: u32, x2: u32, y2: u32, color: u32, fill: bool);
    fn xapi_DrawSWText(handle: Hdl, x: u32, y: u32, text: *mut c_char, color: u32);
    fn xapi_ReadBuffer(handle: Hdl, x: u32, y: u32, width: u32, height: u32, buffer: *mut XColor);
    fn xapi_WriteBuffer(handle: Hdl, x: u32, y: u32, width: u32, height: u32, buffer: *mut XColor);
    fn xapi_RefreshWindow(handle: Hdl);
    fn SetMsgPrcor(handle: Hdl, func: MsgProc);
    fn xapi_GetCurrentUser(user_info: *mut UserInfo);

    fn malloc(size: SizeT) -> *mut c_void;
    fn realloc(ptr: *mut c_void, size: SizeT) -> *mut c_void;
    fn exit(status: c_int) -> !;
    fn posix_openpt(flags: c_int) -> c_int;
    fn grantpt(fd: c_int) -> c_int;
    fn unlockpt(fd: c_int) -> c_int;
    fn ptsname(fd: c_int) -> *mut c_char;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut c_void, len: SizeT) -> SSizeT;
    fn write(fd: c_int, buf: *const c_void, len: SizeT) -> SSizeT;
    fn fork() -> c_int;
    fn dup2(fd: c_int, newfd: c_int) -> c_int;
    fn execve(filename: *const c_char, argv: *mut *mut c_char, envp: *mut *mut c_char) -> c_int;
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    fn poll(fds: *mut PollFd, nfds: u64, timeout_ms: c_int) -> c_int;
    fn waitpid(pid: c_int, status: *mut c_int, options: c_int) -> c_int;
    fn clock_gettime(clockid: c_int, ts: *mut Timespec) -> c_int;

    fn vterm_build(builder: *const VTermBuilder) -> *mut VTerm;
    fn vterm_set_size(vt: *mut VTerm, rows: c_int, cols: c_int);
    fn vterm_set_utf8(vt: *mut VTerm, is_utf8: c_int);
    fn vterm_input_write(vt: *mut VTerm, bytes: *const c_char, len: SizeT) -> SizeT;
    fn vterm_output_set_callback(
        vt: *mut VTerm,
        func: Option<extern "C" fn(*const c_char, SizeT, *mut c_void)>,
        user: *mut c_void,
    );
    fn vterm_obtain_screen(vt: *mut VTerm) -> *mut VTermScreen;
    fn vterm_screen_set_default_colors(screen: *mut VTermScreen, fg: *const VTermColor, bg: *const VTermColor);
    fn vterm_screen_set_callbacks(screen: *mut VTermScreen, callbacks: *const VTermScreenCallbacks, user: *mut c_void);
    fn vterm_screen_set_damage_merge(screen: *mut VTermScreen, size: c_int);
    fn vterm_screen_enable_altscreen(screen: *mut VTermScreen, altscreen: c_int);
    fn vterm_screen_reset(screen: *mut VTermScreen, hard: c_int);
    fn vterm_screen_get_cell(screen: *mut VTermScreen, pos: VTermPos, cell: *mut VTermScreenCell) -> c_int;
    fn vterm_screen_flush_damage(screen: *mut VTermScreen);
}

fn clamp_i(v: i32, lo: i32, hi: i32) -> i32 {
    if v < lo { lo } else if v > hi { hi } else { v }
}

fn current_language() -> c_int {
    unsafe { G_LANGUAGE }
}

fn tr_bytes(zh_cn: &'static [u8], en_us: &'static [u8]) -> &'static [u8] {
    rust_i18n::tr_bytes_lang(current_language(), zh_cn, en_us)
}

fn tr_cstr(zh_cn: *const c_char, en_us: *const c_char) -> *mut c_char {
    rust_i18n::tr_cstr_lang(current_language(), zh_cn, en_us)
}

fn c_strlen(s: *const c_char) -> usize {
    if s.is_null() {
        return 0;
    }
    let mut len = 0usize;
    unsafe {
        while *s.add(len) != 0 {
            len += 1;
        }
    }
    len
}

fn c_starts_with_env_key(entry: *const c_char, key: &[u8]) -> bool {
    if entry.is_null() {
        return false;
    }
    unsafe {
        let mut i = 0usize;
        while i < key.len() {
            if *entry.add(i) as u8 != key[i] {
                return false;
            }
            i += 1;
        }
        *entry.add(key.len()) as u8 == b'='
    }
}

fn env_count(envp: *mut *mut c_char) -> usize {
    if envp.is_null() {
        return 0;
    }
    let mut count = 0usize;
    unsafe {
        while !(*envp.add(count)).is_null() {
            count += 1;
        }
    }
    count
}

fn env_has_key(envp: *mut *mut c_char, key: &[u8]) -> bool {
    if envp.is_null() {
        return false;
    }
    let mut i = 0usize;
    unsafe {
        while !(*envp.add(i)).is_null() {
            if c_starts_with_env_key(*envp.add(i), key) {
                return true;
            }
            i += 1;
        }
    }
    false
}

fn copy_cstr_to_buf(dst: &mut [u8], src: *const c_char) {
    dst.fill(0);
    if src.is_null() || dst.is_empty() {
        return;
    }
    let mut i = 0usize;
    unsafe {
        while i + 1 < dst.len() {
            let ch = *src.add(i) as u8;
            if ch == 0 {
                break;
            }
            dst[i] = ch;
            i += 1;
        }
    }
}

fn append_bytes(dst: &mut [u8], pos: &mut usize, bytes: &[u8]) {
    for &b in bytes {
        if *pos + 1 >= dst.len() {
            break;
        }
        dst[*pos] = b;
        *pos += 1;
    }
    if !dst.is_empty() {
        let nul = if *pos < dst.len() { *pos } else { dst.len() - 1 };
        dst[nul] = 0;
    }
}

fn append_cstr(dst: &mut [u8], pos: &mut usize, s: *const c_char) {
    if s.is_null() {
        return;
    }
    let mut i = 0usize;
    unsafe {
        loop {
            let ch = *s.add(i) as u8;
            if ch == 0 {
                break;
            }
            append_bytes(dst, pos, &[ch]);
            i += 1;
        }
    }
}

fn append_i32(dst: &mut [u8], pos: &mut usize, mut value: i32) {
    if value == 0 {
        append_bytes(dst, pos, b"0");
        return;
    }
    if value < 0 {
        append_bytes(dst, pos, b"-");
        value = value.wrapping_neg();
    }
    let mut tmp = [0u8; 12];
    let mut len = 0usize;
    let mut n = value as u32;
    while n > 0 && len < tmp.len() {
        tmp[len] = b'0' + (n % 10) as u8;
        n /= 10;
        len += 1;
    }
    while len > 0 {
        len -= 1;
        append_bytes(dst, pos, &[tmp[len]]);
    }
}

fn build_kv<const N: usize>(prefix: &[u8], value: *const c_char) -> [u8; N] {
    let mut out = [0u8; N];
    let mut pos = 0usize;
    append_bytes(&mut out, &mut pos, prefix);
    append_cstr(&mut out, &mut pos, value);
    out
}

fn get_current_user_name(out: &mut [u8; 64]) {
    let mut info = UserInfo { name: [0; 64], user_type: 0 };
    unsafe {
        xapi_GetCurrentUser(&mut info);
    }
    if info.name[0] != 0 {
        let src = info.name.as_ptr();
        copy_cstr_to_buf(out, src);
    } else {
        out.fill(0);
        let mut pos = 0usize;
        append_bytes(out, &mut pos, b"Root");
    }
}

fn build_shell_env(envp: *mut *mut c_char) -> *mut *mut c_char {
    unsafe {
        get_current_user_name(&mut ENV_USERNAME);
        let username_ptr = ENV_USERNAME.as_ptr() as *const c_char;

        ENV_HOME = build_kv::<104>(b"HOME=/users/", username_ptr);
        ENV_USER = build_kv::<80>(b"USER=", username_ptr);
        ENV_LOGNAME = build_kv::<88>(b"LOGNAME=", username_ptr);
        ENV_PS1.fill(0);
        let mut ps1_pos = 0usize;
        append_bytes(&mut ENV_PS1, &mut ps1_pos, b"PS1=\\[\\033[01;32m\\]");
        append_cstr(&mut ENV_PS1, &mut ps1_pos, username_ptr);
        append_bytes(
            &mut ENV_PS1,
            &mut ps1_pos,
            b"@\\h\\[\\033[00m\\]:\\[\\033[01;34m\\]\\w\\[\\033[00m\\]\\\\$ ",
        );

        let extras: [(&[u8], *mut c_char); 9] = [
            (b"TERM", c"TERM=xterm-256color".as_ptr() as *mut c_char),
            (b"HOME", ENV_HOME.as_mut_ptr() as *mut c_char),
            (b"USER", ENV_USER.as_mut_ptr() as *mut c_char),
            (b"LOGNAME", ENV_LOGNAME.as_mut_ptr() as *mut c_char),
            (b"HOSTNAME", c"HOSTNAME=localhost".as_ptr() as *mut c_char),
            (b"PATH", c"PATH=/apps:/system:/bin:/usr/bin".as_ptr() as *mut c_char),
            (b"SHELL", c"SHELL=/bin/sh".as_ptr() as *mut c_char),
            (b"PWD", c"PWD=/apps".as_ptr() as *mut c_char),
            (b"PS1", ENV_PS1.as_mut_ptr() as *mut c_char),
        ];

        let base_count = env_count(envp);
        let mut extra_count = 0usize;
        for &(key, _) in &extras {
            if !env_has_key(envp, key) {
                extra_count += 1;
            }
        }

        let total = base_count + extra_count + 1;
        let child_env = malloc(total * core::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
        if child_env.is_null() {
            return envp;
        }

        let mut out = 0usize;
        for i in 0..base_count {
            *child_env.add(out) = *envp.add(i);
            out += 1;
        }
        for &(key, value) in &extras {
            if !env_has_key(envp, key) {
                *child_env.add(out) = value;
                out += 1;
            }
        }
        *child_env.add(out) = ptr::null_mut();
        child_env
    }
}

fn rgb_color(mut r: i32, mut g: i32, mut b: i32) -> u32 {
    r = clamp_i(r, 0, 255);
    g = clamp_i(g, 0, 255);
    b = clamp_i(b, 0, 255);
    ((r as u32) << 24) | ((g as u32) << 16) | ((b as u32) << 8) | 0xff
}

fn xterm_256_color(index: i32) -> u32 {
    let index = clamp_i(index, 0, 255);
    if index < 16 {
        return PALETTE[index as usize];
    }
    if index < 232 {
        let levels = [0, 95, 135, 175, 215, 255];
        let n = index - 16;
        return rgb_color(levels[(n / 36) as usize], levels[((n / 6) % 6) as usize], levels[(n % 6) as usize]);
    }
    let gray = 8 + (index - 232) * 10;
    rgb_color(gray, gray, gray)
}

fn monotonic_ns() -> u64 {
    let mut ts = Timespec { tv_sec: 0, tv_nsec: 0 };
    let ret = unsafe { clock_gettime(CLOCK_MONOTONIC, &mut ts) };
    if ret < 0 {
        return 0;
    }
    (ts.tv_sec as u64) * 1_000_000_000 + ts.tv_nsec as u64
}

fn ensure_scroll_buffer(width: i32, height: i32) -> bool {
    if width <= 0 || height <= 0 {
        return false;
    }
    let pixels = width as usize * height as usize;
    unsafe {
        if pixels <= G_SCROLL_BUFFER_PIXELS && !G_SCROLL_BUFFER.is_null() {
            return true;
        }
        let buffer = realloc(G_SCROLL_BUFFER as *mut c_void, pixels * core::mem::size_of::<XColor>()) as *mut XColor;
        if buffer.is_null() {
            return false;
        }
        G_SCROLL_BUFFER = buffer;
        G_SCROLL_BUFFER_PIXELS = pixels;
    }
    true
}

fn calculate_terminal_geometry(width: i32, height: i32) -> (i32, i32) {
    let min_width = TERM_LEFT_X + TERM_RIGHT_PAD + CELL_W;
    let min_height = TERM_TOP_Y + TERM_BOTTOM_PAD + CELL_H;
    let width = if width < min_width { min_width } else { width };
    let height = if height < min_height { min_height } else { height };
    let cols = ((width - TERM_LEFT_X - TERM_RIGHT_PAD) / CELL_W).max(1);
    let rows = ((height - TERM_TOP_Y - TERM_BOTTOM_PAD) / CELL_H).max(1);
    (cols, rows)
}

fn sync_pty_window_size() {
    unsafe {
        if G_PTY_MASTER < 0 {
            return;
        }
        let mut ws = Winsize {
            ws_row: G_TERM_ROWS as u16,
            ws_col: G_TERM_COLS as u16,
            ws_xpixel: (G_TERM_COLS * CELL_W) as u16,
            ws_ypixel: (G_TERM_ROWS * CELL_H) as u16,
        };
        ioctl(G_PTY_MASTER, TIOCSWINSZ, &mut ws);
    }
}

fn fill_rect(mut x1: i32, mut y1: i32, mut x2: i32, mut y2: i32, color: u32) {
    unsafe {
        x1 = clamp_i(x1, 0, G_WIN_WIDTH - 1);
        y1 = clamp_i(y1, 0, G_WIN_HEIGHT - 1);
        x2 = clamp_i(x2, 0, G_WIN_WIDTH - 1);
        y2 = clamp_i(y2, 0, G_WIN_HEIGHT - 1);
        if x2 < x1 || y2 < y1 {
            return;
        }
        xapi_DrawRect(G_WINDOW, x1 as u32, y1 as u32, x2 as u32, y2 as u32, color, true);
    }
}

fn clear_window() {
    unsafe {
        xapi_DrawRect(G_WINDOW, 0, 0, (G_WIN_WIDTH - 1) as u32, (G_WIN_HEIGHT - 1) as u32, COLOR_BG, true);
    }
}

fn clear_resized_edges(old_width: i32, old_height: i32) {
    unsafe {
        if G_WIN_WIDTH > old_width {
            fill_rect(old_width, 0, G_WIN_WIDTH - 1, G_WIN_HEIGHT - 1, COLOR_BG);
        }
        if G_WIN_HEIGHT > old_height {
            fill_rect(0, old_height, G_WIN_WIDTH - 1, G_WIN_HEIGHT - 1, COLOR_BG);
        }
    }
}

fn send_child(buf: *const c_char, len: i32) {
    unsafe {
        if buf.is_null() || len <= 0 || G_PTY_MASTER < 0 {
            return;
        }
        write(G_PTY_MASTER, buf as *const c_void, len as usize);
    }
}

fn utf8_encode(cp: u32, out: &mut [u8; 5]) {
    out.fill(0);
    if cp == 0 {
        return;
    }
    if cp < 0x80 {
        out[0] = cp as u8;
    } else if cp < 0x800 {
        out[0] = (0xc0 | (cp >> 6)) as u8;
        out[1] = (0x80 | (cp & 0x3f)) as u8;
    } else if cp < 0x10000 {
        out[0] = (0xe0 | (cp >> 12)) as u8;
        out[1] = (0x80 | ((cp >> 6) & 0x3f)) as u8;
        out[2] = (0x80 | (cp & 0x3f)) as u8;
    } else {
        out[0] = (0xf0 | (cp >> 18)) as u8;
        out[1] = (0x80 | ((cp >> 12) & 0x3f)) as u8;
        out[2] = (0x80 | ((cp >> 6) & 0x3f)) as u8;
        out[3] = (0x80 | (cp & 0x3f)) as u8;
    }
}

fn vterm_color_type(color: &VTermColor) -> u8 {
    unsafe { color.type_ }
}

fn vterm_color_to_xcolor(color: VTermColor, fallback: u32) -> u32 {
    let type_ = vterm_color_type(&color);
    if (type_ & (VTERM_COLOR_DEFAULT_FG | VTERM_COLOR_DEFAULT_BG)) != 0 {
        return fallback;
    }
    if (type_ & VTERM_COLOR_TYPE_MASK) == VTERM_COLOR_INDEXED {
        return unsafe { xterm_256_color(color.indexed.idx as i32) };
    }
    unsafe { rgb_color(color.rgb.red as i32, color.rgb.green as i32, color.rgb.blue as i32) }
}

fn draw_vterm_cell(row: i32, col: i32) {
    unsafe {
        if G_VSCREEN.is_null() || row < 0 || row >= G_TERM_ROWS || col < 0 || col >= G_TERM_COLS {
            return;
        }

        let mut cell = core::mem::MaybeUninit::<VTermScreenCell>::zeroed().assume_init();
        let pos = VTermPos { row, col };
        if vterm_screen_get_cell(G_VSCREEN, pos, &mut cell) == 0 {
            return;
        }

        let mut fg = vterm_color_to_xcolor(cell.fg, COLOR_FG);
        let mut bg = vterm_color_to_xcolor(cell.bg, COLOR_BG);
        let attrs = cell.attrs;
        let bold = (attrs & (1 << 0)) != 0;
        let reverse = (attrs & (1 << 5)) != 0;
        let conceal = (attrs & (1 << 6)) != 0;
        if reverse {
            core::mem::swap(&mut fg, &mut bg);
        }
        if bold && (vterm_color_type(&cell.fg) & VTERM_COLOR_TYPE_MASK) == VTERM_COLOR_INDEXED && cell.fg.indexed.idx < 8 {
            fg = xterm_256_color(cell.fg.indexed.idx as i32 + 8);
        }

        let x = TERM_LEFT_X + col * CELL_W;
        let y = TERM_TOP_Y + row * CELL_H;
        let width_cells = if cell.width > 1 { cell.width as i32 } else { 1 };
        let w = width_cells * CELL_W;
        fill_rect(x, y, x + w - 1, y + CELL_H - 1, bg);

        if !conceal && cell.chars[0] != 0 {
            let mut text = [0u8; VTERM_MAX_CHARS_PER_CELL * 4 + 1];
            let mut out = 0usize;
            for i in 0..VTERM_MAX_CHARS_PER_CELL {
                let ch = cell.chars[i];
                if ch == 0 {
                    break;
                }
                let mut one = [0u8; 5];
                utf8_encode(ch, &mut one);
                for &b in &one {
                    if b == 0 || out + 1 >= text.len() {
                        break;
                    }
                    text[out] = b;
                    out += 1;
                }
            }
            if out > 0 {
                xapi_DrawSWText(G_WINDOW, x as u32, y as u32, text.as_mut_ptr() as *mut c_char, fg);
            }
        }

        if G_CURSOR_VISIBLE && row == G_CURSOR.row && col == G_CURSOR.col {
            fill_rect(x, y + CELL_H - 2, x + CELL_W - 1, y + CELL_H - 1, fg);
        }
    }
}

fn redraw_vterm_rect(mut rect: VTermRect) {
    unsafe {
        rect.start_row = clamp_i(rect.start_row, 0, G_TERM_ROWS);
        rect.end_row = clamp_i(rect.end_row, 0, G_TERM_ROWS);
        rect.start_col = clamp_i(rect.start_col, 0, G_TERM_COLS);
        rect.end_col = clamp_i(rect.end_col, 0, G_TERM_COLS);
    }
    let mut r = rect.start_row;
    while r < rect.end_row {
        let mut c = rect.start_col;
        while c < rect.end_col {
            draw_vterm_cell(r, c);
            c += 1;
        }
        r += 1;
    }
}

extern "C" fn vterm_damage(rect: VTermRect, _user: *mut c_void) -> c_int {
    redraw_vterm_rect(rect);
    1
}

extern "C" fn vterm_moverect(dest: VTermRect, src: VTermRect, _user: *mut c_void) -> c_int {
    unsafe {
        let src_x = TERM_LEFT_X + src.start_col * CELL_W;
        let src_y = TERM_TOP_Y + src.start_row * CELL_H;
        let dst_x = TERM_LEFT_X + dest.start_col * CELL_W;
        let dst_y = TERM_TOP_Y + dest.start_row * CELL_H;
        let width = (src.end_col - src.start_col) * CELL_W;
        let height = (src.end_row - src.start_row) * CELL_H;
        if width <= 0 || height <= 0 {
            return 0;
        }
        if src_x < 0 || src_y < 0 || src_x + width > G_WIN_WIDTH || src_y + height > G_WIN_HEIGHT {
            return 0;
        }
        if dst_x < 0 || dst_y < 0 || dst_x + width > G_WIN_WIDTH || dst_y + height > G_WIN_HEIGHT {
            return 0;
        }
        if !ensure_scroll_buffer(width, height) {
            return 0;
        }

        xapi_ReadBuffer(G_WINDOW, src_x as u32, src_y as u32, width as u32, height as u32, G_SCROLL_BUFFER);
        xapi_WriteBuffer(G_WINDOW, dst_x as u32, dst_y as u32, width as u32, height as u32, G_SCROLL_BUFFER);

        if dest.start_row < src.start_row {
            redraw_vterm_rect(VTermRect {
                start_row: dest.end_row,
                end_row: src.end_row,
                start_col: dest.start_col,
                end_col: dest.end_col,
            });
        } else if dest.start_row > src.start_row {
            redraw_vterm_rect(VTermRect {
                start_row: src.start_row,
                end_row: dest.start_row,
                start_col: dest.start_col,
                end_col: dest.end_col,
            });
        }
        if G_CURSOR_VISIBLE {
            draw_vterm_cell(G_CURSOR.row, G_CURSOR.col);
        }
    }
    1
}

extern "C" fn vterm_movecursor(pos: VTermPos, oldpos: VTermPos, visible: c_int, _user: *mut c_void) -> c_int {
    unsafe {
        let previous = G_CURSOR;
        G_CURSOR = pos;
        G_CURSOR_VISIBLE = visible != 0;
        draw_vterm_cell(oldpos.row, oldpos.col);
        draw_vterm_cell(previous.row, previous.col);
        draw_vterm_cell(G_CURSOR.row, G_CURSOR.col);
    }
    1
}

extern "C" fn vterm_settermprop(prop: c_int, val: *mut VTermValue, _user: *mut c_void) -> c_int {
    unsafe {
        if prop == VTERM_PROP_CURSORVISIBLE && !val.is_null() {
            G_CURSOR_VISIBLE = (*val).boolean != 0;
            draw_vterm_cell(G_CURSOR.row, G_CURSOR.col);
        }
    }
    1
}

extern "C" fn vterm_output(s: *const c_char, len: SizeT, _user: *mut c_void) {
    send_child(s, len as i32);
}

fn vterm_color_rgb(red: u8, green: u8, blue: u8) -> VTermColor {
    VTermColor { rgb: VTermColorRgb { type_: 0, red, green, blue } }
}

fn terminal_init() {
    unsafe {
        if !G_VTERM.is_null() {
            return;
        }
        let builder = VTermBuilder {
            ver: 0,
            rows: G_TERM_ROWS,
            cols: G_TERM_COLS,
            allocator: ptr::null(),
            allocdata: ptr::null_mut(),
            outbuffer_len: 0,
            tmpbuffer_len: 0,
        };
        G_VTERM = vterm_build(&builder);
        if G_VTERM.is_null() {
            return;
        }
        vterm_set_utf8(G_VTERM, 1);
        vterm_output_set_callback(G_VTERM, Some(vterm_output), ptr::null_mut());
        G_VSCREEN = vterm_obtain_screen(G_VTERM);
        if G_VSCREEN.is_null() {
            return;
        }

        let fg = vterm_color_rgb(0xe6, 0xed, 0xf3);
        let bg = vterm_color_rgb(0x0b, 0x0f, 0x14);
        vterm_screen_set_default_colors(G_VSCREEN, &fg, &bg);
        vterm_screen_set_callbacks(G_VSCREEN, &VTERM_CALLBACKS, ptr::null_mut());
        vterm_screen_set_damage_merge(G_VSCREEN, VTERM_DAMAGE_SCROLL);
        vterm_screen_enable_altscreen(G_VSCREEN, 1);
        vterm_screen_reset(G_VSCREEN, 1);
    }
}

fn terminal_resize(width: i32, height: i32) {
    unsafe {
        let old_width = G_WIN_WIDTH;
        let old_height = G_WIN_HEIGHT;
        let old_cols = G_TERM_COLS;
        let old_rows = G_TERM_ROWS;
        let (new_cols, new_rows) = calculate_terminal_geometry(width, height);

        G_WIN_WIDTH = width;
        G_WIN_HEIGHT = height;
        let min_width = TERM_LEFT_X + TERM_RIGHT_PAD + CELL_W;
        let min_height = TERM_TOP_Y + TERM_BOTTOM_PAD + CELL_H;
        if G_WIN_WIDTH < min_width {
            G_WIN_WIDTH = min_width;
        }
        if G_WIN_HEIGHT < min_height {
            G_WIN_HEIGHT = min_height;
        }

        if new_cols != G_TERM_COLS || new_rows != G_TERM_ROWS {
            G_TERM_COLS = new_cols;
            G_TERM_ROWS = new_rows;
            if !G_VTERM.is_null() {
                vterm_set_size(G_VTERM, G_TERM_ROWS, G_TERM_COLS);
            }
        }

        G_CURSOR.row = clamp_i(G_CURSOR.row, 0, G_TERM_ROWS - 1);
        G_CURSOR.col = clamp_i(G_CURSOR.col, 0, G_TERM_COLS - 1);
        sync_pty_window_size();
        if G_TERMINAL_PAINTED && new_cols == old_cols && new_rows == old_rows {
            clear_resized_edges(old_width, old_height);
            if G_CURSOR_VISIBLE {
                draw_vterm_cell(G_CURSOR.row, G_CURSOR.col);
            }
            return;
        }

        clear_window();
        if !G_VSCREEN.is_null() {
            redraw_vterm_rect(VTermRect {
                start_row: 0,
                end_row: G_TERM_ROWS,
                start_col: 0,
                end_col: G_TERM_COLS,
            });
        }
        G_TERMINAL_PAINTED = true;
    }
}

fn terminal_write_chunk(buf: *const c_char, len: i32, flush_damage: bool) -> bool {
    if buf.is_null() || len <= 0 {
        return false;
    }
    unsafe {
        if G_VTERM.is_null() {
            terminal_init();
        }
        if G_VTERM.is_null() {
            xapi_DrawSWText(
                G_WINDOW,
                TERM_LEFT_X as u32,
                TERM_TOP_Y as u32,
                tr_cstr(
                    c"busyterm：libvterm 初始化失败".as_ptr(),
                    c"busyterm: libvterm initialization failed".as_ptr(),
                ),
                COLOR_FG,
            );
            return true;
        }
        vterm_input_write(G_VTERM, buf, len as usize);
        if flush_damage && !G_VSCREEN.is_null() {
            vterm_screen_flush_damage(G_VSCREEN);
        }
    }
    true
}

fn terminal_flush_damage() {
    unsafe {
        if !G_VSCREEN.is_null() {
            vterm_screen_flush_damage(G_VSCREEN);
        }
    }
}

fn terminal_write(buf: *const c_char, len: i32) {
    terminal_write_chunk(buf, len, true);
}

fn queue_terminal_resize(width: i32, height: i32) {
    unsafe {
        G_RESIZE_PENDING_WIDTH = width;
        G_RESIZE_PENDING_HEIGHT = height;
        G_RESIZE_LAST_EVENT_NS = monotonic_ns();
        G_RESIZE_PENDING = true;
    }
}

fn service_pending_resize(force: bool) {
    unsafe {
        if !G_RESIZE_PENDING {
            return;
        }
        let now = monotonic_ns();
        if !force && now != 0 && G_RESIZE_LAST_EVENT_NS != 0 && now - G_RESIZE_LAST_EVENT_NS < RESIZE_DEBOUNCE_NS {
            return;
        }
        G_RESIZE_PENDING = false;
        terminal_resize(G_RESIZE_PENDING_WIDTH, G_RESIZE_PENDING_HEIGHT);
        xapi_RefreshWindow(G_WINDOW);
    }
}

fn send_key_seq(key: u64) {
    let seq = match key {
        TERM_KEY_ESC => c"\x1b".as_ptr(),
        TERM_KEY_TAB => c"\t".as_ptr(),
        TERM_KEY_F1 => c"\x1bOP".as_ptr(),
        TERM_KEY_F2 => c"\x1bOQ".as_ptr(),
        TERM_KEY_F3 => c"\x1bOR".as_ptr(),
        TERM_KEY_F4 => c"\x1bOS".as_ptr(),
        TERM_KEY_F5 => c"\x1b[15~".as_ptr(),
        TERM_KEY_F6 => c"\x1b[17~".as_ptr(),
        TERM_KEY_F7 => c"\x1b[18~".as_ptr(),
        TERM_KEY_F8 => c"\x1b[19~".as_ptr(),
        TERM_KEY_F9 => c"\x1b[20~".as_ptr(),
        TERM_KEY_F10 => c"\x1b[21~".as_ptr(),
        TERM_KEY_F11 => c"\x1b[23~".as_ptr(),
        TERM_KEY_F12 => c"\x1b[24~".as_ptr(),
        TERM_KEY_HOME => c"\x1b[H".as_ptr(),
        TERM_KEY_UP => c"\x1b[A".as_ptr(),
        TERM_KEY_PAGE_UP => c"\x1b[5~".as_ptr(),
        TERM_KEY_LEFT => c"\x1b[D".as_ptr(),
        TERM_KEY_RIGHT => c"\x1b[C".as_ptr(),
        TERM_KEY_END => c"\x1b[F".as_ptr(),
        TERM_KEY_DOWN => c"\x1b[B".as_ptr(),
        TERM_KEY_PAGE_DOWN => c"\x1b[6~".as_ptr(),
        TERM_KEY_INSERT => c"\x1b[2~".as_ptr(),
        TERM_KEY_DELETE => c"\x1b[3~".as_ptr(),
        _ => return,
    };
    send_child(seq, c_strlen(seq) as i32);
}

fn ctrl_char(input: u8) -> Option<u8> {
    if input.is_ascii_lowercase() {
        return Some(input - b'a' + 1);
    }
    if input.is_ascii_uppercase() {
        return Some(input - b'A' + 1);
    }
    match input {
        b' ' | b'@' => Some(0x00),
        b'[' => Some(0x1b),
        b'\\' => Some(0x1c),
        b']' => Some(0x1d),
        b'^' | b'6' => Some(0x1e),
        b'_' | b'-' => Some(0x1f),
        b'?' => Some(0x7f),
        _ => None,
    }
}

extern "C" fn busyterm_msg(type_: u64, h_data: u64, l_data: u64) {
    let _ = h_data;
    unsafe {
        if type_ == MSG_CHAR {
            let c = l_data as u8;
            if G_CTRL_DOWN {
                if let Some(ctrl) = ctrl_char(c) {
                    let one = [ctrl];
                    send_child(one.as_ptr() as *const c_char, 1);
                    G_CTRL_DOWN = false;
                    return;
                }
            }
            let one = [c];
            send_child(one.as_ptr() as *const c_char, 1);
        } else if type_ == MSG_SPCHAR {
            if l_data == TERM_KEY_CTRL {
                G_CTRL_DOWN = true;
                return;
            }
            if l_data == TERM_KEY_CTRL_RELEASE {
                G_CTRL_DOWN = false;
                return;
            }
            if l_data == TERM_KEY_SHIFT || l_data == TERM_KEY_ALT {
                return;
            }
            if l_data == b'\n' as u64 {
                let one = [b'\n'];
                send_child(one.as_ptr() as *const c_char, 1);
            } else if l_data == b'\x08' as u64 {
                let one = [0x7f];
                send_child(one.as_ptr() as *const c_char, 1);
            } else {
                send_key_seq(l_data);
            }
        } else if type_ == MSG_RESIZE {
            queue_terminal_resize(h_data as i32, l_data as i32);
        }
    }
}

fn spawn_busybox(envp: *mut *mut c_char) -> c_int {
    unsafe {
        G_SPAWN_ERROR = 0;
        G_SPAWN_STAGE = c"posix_openpt".as_ptr();
        G_PTY_MASTER = posix_openpt(O_RDWR | O_NOCTTY);
        if G_PTY_MASTER < 0 {
            G_SPAWN_ERROR = errno;
            return -1;
        }
        G_SPAWN_STAGE = c"grantpt".as_ptr();
        if grantpt(G_PTY_MASTER) < 0 {
            G_SPAWN_ERROR = errno;
            close(G_PTY_MASTER);
            G_PTY_MASTER = -1;
            return -1;
        }
        G_SPAWN_STAGE = c"unlockpt".as_ptr();
        if unlockpt(G_PTY_MASTER) < 0 {
            G_SPAWN_ERROR = errno;
            close(G_PTY_MASTER);
            G_PTY_MASTER = -1;
            return -1;
        }

        G_SPAWN_STAGE = c"ptsname".as_ptr();
        let slave_name = ptsname(G_PTY_MASTER);
        if slave_name.is_null() {
            G_SPAWN_ERROR = errno;
            close(G_PTY_MASTER);
            G_PTY_MASTER = -1;
            return -1;
        }

        G_SPAWN_STAGE = c"fork".as_ptr();
        sync_pty_window_size();
        let pid = fork();
        if pid == 0 {
            let child_env = build_shell_env(envp);
            let slave = open(slave_name, O_RDWR | O_NOCTTY);
            if slave < 0 {
                exit(126);
            }
            ioctl(slave, TIOCSCTTY, 0);
            dup2(slave, 0);
            dup2(slave, 1);
            dup2(slave, 2);
            if slave > 2 {
                close(slave);
            }
            close(G_PTY_MASTER);

            let mut shell_argv = [c"sh".as_ptr() as *mut c_char, c"-i".as_ptr() as *mut c_char, ptr::null_mut()];
            execve(DEFAULT_SHELL, shell_argv.as_mut_ptr(), child_env);
            execve(BUSYBOX_PATH, shell_argv.as_mut_ptr(), child_env);
            exit(127);
        }
        if pid < 0 {
            G_SPAWN_ERROR = errno;
            close(G_PTY_MASTER);
            G_PTY_MASTER = -1;
            return -1;
        }
        G_CHILD_PID = pid;
        pid
    }
}

fn drain_pty_output() {
    unsafe {
        if G_PTY_MASTER < 0 {
            return;
        }
        let mut pfd = PollFd {
            fd: G_PTY_MASTER,
            events: POLLIN | POLLHUP,
            revents: 0,
        };
        let mut drew = false;
        loop {
            let ready = poll(&mut pfd, 1, 0);
            if ready <= 0 || (pfd.revents & (POLLIN | POLLHUP)) == 0 {
                break;
            }
            let mut output = [0u8; PTY_CHUNK];
            let got = read(G_PTY_MASTER, output.as_mut_ptr() as *mut c_void, output.len());
            if got > 0 {
                drew |= terminal_write_chunk(output.as_ptr() as *const c_char, got as i32, false);
            } else {
                break;
            }
            if (pfd.revents & POLLHUP) != 0 {
                break;
            }
            pfd.revents = 0;
        }
        if drew {
            terminal_flush_damage();
            xapi_RefreshWindow(G_WINDOW);
        }
    }
}

fn write_spawn_error() {
    let mut msg = [0u8; 128];
    let mut pos = 0usize;
    append_bytes(
        &mut msg,
        &mut pos,
        tr_bytes(
            "busyterm：启动 Shell 失败，阶段 ".as_bytes(),
            b"busyterm: failed to start Shell at stage ",
        ),
    );
    unsafe {
        append_cstr(&mut msg, &mut pos, G_SPAWN_STAGE);
    }
    append_bytes(&mut msg, &mut pos, tr_bytes(" 错误码=".as_bytes(), b" error="));
    unsafe {
        append_i32(&mut msg, &mut pos, G_SPAWN_ERROR);
    }
    append_bytes(&mut msg, &mut pos, b"\n");
    terminal_write(msg.as_ptr() as *const c_char, c_strlen(msg.as_ptr() as *const c_char) as i32);
}

#[unsafe(no_mangle)]
pub extern "C" fn main(_argc: c_int, _argv: *mut *mut c_char, envp: *mut *mut c_char) -> c_int {
    unsafe {
        G_LANGUAGE = rust_i18n::read_language();
        let mut win = XWindow {
            width: BUSY_WIDTH as u32,
            height: BUSY_HEIGHT as u32,
            title: tr_cstr(c"终端".as_ptr(), c"Terminal".as_ptr()),
            sets: XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE,
        };
        xapi_CreateWindow(&raw mut G_WINDOW, &mut win);
        xapi_SetIcon(G_WINDOW, c"/system/icon/terminal.png".as_ptr() as *mut c_char);
        SetMsgPrcor(G_WINDOW, busyterm_msg);
        let (cols, rows) = calculate_terminal_geometry(BUSY_WIDTH, BUSY_HEIGHT);
        G_TERM_COLS = cols;
        G_TERM_ROWS = rows;
        terminal_init();
        terminal_resize(BUSY_WIDTH, BUSY_HEIGHT);
        xapi_RefreshWindow(G_WINDOW);

        if spawn_busybox(envp) < 0 {
            write_spawn_error();
            xapi_RefreshWindow(G_WINDOW);
        }

        loop {
            service_pending_resize(false);
            drain_pty_output();
            if G_CHILD_PID > 0 {
                let mut status = 0;
                if waitpid(G_CHILD_PID, &mut status, WNOHANG) == G_CHILD_PID {
                    G_CHILD_PID = -1;
                    let exited = tr_bytes("\n[Shell 已退出]\n".as_bytes(), b"\n[Shell exited]\n");
                    terminal_write(exited.as_ptr() as *const c_char, exited.len() as i32);
                    xapi_RefreshWindow(G_WINDOW);
                    if G_PTY_MASTER >= 0 {
                        close(G_PTY_MASTER);
                        G_PTY_MASTER = -1;
                    }
                }
            }
            core::arch::asm!("pause");
        }
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
