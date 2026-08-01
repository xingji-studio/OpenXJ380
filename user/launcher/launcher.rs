#![no_std]
#![allow(static_mut_refs)]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

mod xapi_keys;
#[path = "../rust_i18n.rs"]
mod rust_i18n;

use crate::xapi_keys::{XKEY_DOWN, XKEY_ENTER, XKEY_ESC, XKEY_UP, XPOWER_REBOOT, XPOWER_SHUTDOWN, XWIN_FRAME_OFF};

type Hdl = u64;
type MsgProc = extern "C" fn(u64, u64, u64);

const WINDOW_WIDTH: u32 = 720;
const WINDOW_HEIGHT: u32 = 430;
const MAX_QUERY: usize = 128;
const MAX_RESULTS: usize = 10;
const MAX_FILE_RESULTS: usize = 96;
const MAX_CANDIDATES: usize = 128;
const MAX_RECENTS: usize = 12;
const RECENT_FILE_BYTES: usize = 2048;
const PATH_CAP: usize = 256;
const LABEL_CAP: usize = 96;
const DIRNODE_COUNT: usize = 256;
const ROW_TOP: i32 = 86;
const ROW_HEIGHT: i32 = 30;

const MSG_CHAR: u64 = 0;
const MSG_LBUTTON: u64 = 2;
const MSG_CRL: u64 = 6;
const MSG_SPCHAR: u64 = 7;
const MSG_KEYDOWN: u64 = 10;

const COLOR_BG: u32 = 0xf7f9fcff;
const COLOR_PANEL: u32 = 0xffffffff;
const COLOR_ACCENT: u32 = 0x2878f0ff;
const COLOR_ROW_HOVER: u32 = 0xe8f1ffff;
const COLOR_TEXT: u32 = 0x152235ff;
const COLOR_MUTED: u32 = 0x607085ff;
const COLOR_LINE: u32 = 0xd8e0ebff;
const COLOR_ERROR: u32 = 0xc0342bff;

#[repr(C)]
struct XWindow {
    width: u32,
    height: u32,
    title: *mut c_char,
    sets: u8,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct DirNode {
    filename: [u8; 256],
    length: u64,
    filetype: u64,
}

#[repr(C)]
struct XFile {
    length: u64,
    buffer: *mut u8,
}

#[derive(Copy, Clone)]
struct StaticEntry {
    title_zh: &'static [u8],
    title_en: &'static [u8],
    subtitle_zh: &'static [u8],
    subtitle_en: &'static [u8],
    kind: EntryKind,
    path: &'static [u8],
    arg: &'static [u8],
    keywords: &'static [u8],
}

#[derive(Copy, Clone)]
struct FileResult {
    label: [u8; LABEL_CAP],
    path: [u8; PATH_CAP],
    filetype: u64,
    kind: EntryKind,
}

#[derive(Copy, Clone)]
struct RecentItem {
    path: [u8; PATH_CAP],
    filetype: u64,
}

#[derive(Copy, Clone)]
struct Candidate {
    item: EntryRef,
    score: i32,
}

#[derive(Copy, Clone)]
enum EntryRef {
    Static(usize),
    File(usize),
    DirectPath,
}

#[derive(Copy, Clone)]
enum EntryKind {
    App,
    Setting,
    Command,
    Recent,
    File,
}

#[derive(Copy, Clone)]
struct LauncherState {
    query: [u8; MAX_QUERY],
    query_len: usize,
    selected: usize,
    result_count: usize,
    results: [EntryRef; MAX_RESULTS],
    file_count: usize,
    file_results: [FileResult; MAX_FILE_RESULTS],
    recent_count: usize,
    recents_loaded: bool,
    recents: [RecentItem; MAX_RECENTS],
    status: [u8; LABEL_CAP],
    redraw: bool,
    exit: bool,
}

impl LauncherState {
    const fn new() -> Self {
        Self {
            query: [0; MAX_QUERY],
            query_len: 0,
            selected: 0,
            result_count: 0,
            results: [EntryRef::DirectPath; MAX_RESULTS],
            file_count: 0,
            file_results: [EMPTY_FILE_RESULT; MAX_FILE_RESULTS],
            recent_count: 0,
            recents_loaded: false,
            recents: [EMPTY_RECENT_ITEM; MAX_RECENTS],
            status: [0; LABEL_CAP],
            redraw: true,
            exit: false,
        }
    }
}

const EMPTY_FILE_RESULT: FileResult = FileResult {
    label: [0; LABEL_CAP],
    path: [0; PATH_CAP],
    filetype: 0,
    kind: EntryKind::File,
};

const EMPTY_RECENT_ITEM: RecentItem = RecentItem {
    path: [0; PATH_CAP],
    filetype: 0,
};

const EMPTY_CANDIDATE: Candidate = Candidate {
    item: EntryRef::DirectPath,
    score: -1,
};

const EMPTY_DIRNODE: DirNode = DirNode {
    filename: [0; 256],
    length: 0,
    filetype: 0,
};

unsafe extern "C" {
    fn xapi_CreateWindow(handle: *mut Hdl, window: *mut XWindow);
    fn xapi_CloseWindow(handle: Hdl);
    fn xapi_SetIcon(handle: Hdl, path: *mut c_char);
    fn xapi_DrawRect(handle: Hdl, x1: u32, y1: u32, x2: u32, y2: u32, color: u32, fill: bool);
    fn xapi_DrawText(handle: Hdl, x: u32, y: u32, text: *mut c_char, size: u32, color: u32);
    fn xapi_DrawFA(handle: Hdl, x: u32, y: u32, width: u32, name: *mut c_char, enable_trans: bool) -> i32;
    fn xapi_RefreshWindow(handle: Hdl);
    fn xapi_SearchFile(path: *mut c_char, count: *mut u32, dir: *mut DirNode);
    fn xapi_OpenFile(path: *mut c_char) -> *mut XFile;
    fn xapi_CloseFile(file: *mut XFile);
    fn xapi_WriteFile(filename: *mut c_char, buffer: *mut c_char, size: u64, offset: u64) -> u64;
    fn xapi_Run(path: *mut c_char);
    fn xapi_RunArgs(path: *mut c_char, argv: *mut *mut c_char) -> u64;
    fn xapi_PowerAction(action: u64) -> u64;
    fn SetMsgPrcor(handle: Hdl, func: MsgProc);
    fn xapi_Sleep(ms: u64);
    fn xapi_Exit(value: u64);
}

static ENTRIES: &[StaticEntry] = &[
    StaticEntry {
        title_zh: b"\xe6\x96\x87\xe4\xbb\xb6\xe8\xb5\x84\xe6\xba\x90\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\0",
        title_en: b"File Manager\0",
        subtitle_zh: b"/apps/system/fmanager.elf\0",
        subtitle_en: b"/apps/system/fmanager.elf\0",
        kind: EntryKind::App,
        path: b"/apps/system/fmanager.elf",
        arg: b"",
        keywords: b"file fmanager explorer wenjian ziyuan guanli qi",
    },
    StaticEntry {
        title_zh: b"\xe7\xbb\x88\xe7\xab\xaf\0",
        title_en: b"Terminal\0",
        subtitle_zh: b"/apps/system/shell.elf\0",
        subtitle_en: b"/apps/system/shell.elf\0",
        kind: EntryKind::App,
        path: b"/apps/system/shell.elf",
        arg: b"",
        keywords: b"terminal shell zhongduan mingling",
    },
    StaticEntry {
        title_zh: b"\xe7\xb3\xbb\xe7\xbb\x9f\xe8\xae\xbe\xe7\xbd\xae\0",
        title_en: b"System Settings\0",
        subtitle_zh: b"\xe6\x89\x93\xe5\xbc\x80\xe8\xae\xbe\xe7\xbd\xae\xe4\xb8\xbb\xe9\xa1\xb5\0",
        subtitle_en: b"Open settings home\0",
        kind: EntryKind::Setting,
        path: b"/apps/system/ctrlmenu.elf",
        arg: b"shortdock-open-settings",
        keywords: b"settings control system shezhi xitong ctrlmenu",
    },
    StaticEntry {
        title_zh: b"\xe5\xa4\x96\xe8\xa7\x82\xe4\xb8\x8e\xe4\xb8\xbb\xe9\xa2\x98\0",
        title_en: b"Appearance & Themes\0",
        subtitle_zh: b"\xe5\x8d\x93\xe9\x9d\xa2\xe3\x80\x81\xe4\xb8\xbb\xe9\xa2\x98\xe5\x92\x8c\xe6\x98\xbe\xe7\xa4\xba\0",
        subtitle_en: b"Desktop, theme, and display\0",
        kind: EntryKind::Setting,
        path: b"/apps/system/ctrlmenu.elf",
        arg: b"shortdock-open-graphics",
        keywords: b"theme display graphics waiguan zhuti zhuomian xianshi",
    },
    StaticEntry {
        title_zh: b"\xe5\x85\xb3\xe4\xba\x8e XJ380\0",
        title_en: b"About XJ380\0",
        subtitle_zh: b"\xe6\x9f\xa5\xe7\x9c\x8b\xe7\xb3\xbb\xe7\xbb\x9f\xe7\x89\x88\xe6\x9c\xac\0",
        subtitle_en: b"View system version\0",
        kind: EntryKind::Setting,
        path: b"/apps/system/xjver.elf",
        arg: b"",
        keywords: b"about xjver version guanyu banben",
    },
    StaticEntry {
        title_zh: b"\xe9\x87\x8d\xe5\x90\xaf\xe7\xb3\xbb\xe7\xbb\x9f\0",
        title_en: b"Restart System\0",
        subtitle_zh: b"\xe7\xab\x8b\xe5\x8d\xb3\xe9\x87\x8d\xe5\x90\xaf XJ380\0",
        subtitle_en: b"Restart XJ380 now\0",
        kind: EntryKind::Command,
        path: b"power:reboot",
        arg: b"",
        keywords: b"reboot restart power chongqi",
    },
    StaticEntry {
        title_zh: b"\xe5\x85\xb3\xe6\x9c\xba\0",
        title_en: b"Shut Down\0",
        subtitle_zh: b"\xe5\x85\xb3\xe9\x97\xad\xe7\x94\xb5\xe6\xba\x90\0",
        subtitle_en: b"Power off\0",
        kind: EntryKind::Command,
        path: b"power:shutdown",
        arg: b"",
        keywords: b"shutdown power off guanji",
    },
    StaticEntry {
        title_zh: b"\xe8\xbf\x90\xe8\xa1\x8c\0",
        title_en: b"Run\0",
        subtitle_zh: b"\xe8\xbe\x93\xe5\x85\xa5 ELF \xe8\xb7\xaf\xe5\xbe\x84\xe6\x88\x96\xe5\x91\xbd\xe4\xbb\xa4\0",
        subtitle_en: b"Enter an ELF path or command\0",
        kind: EntryKind::Command,
        path: b"/apps/system/elfrun.elf",
        arg: b"",
        keywords: b"run elfrun command yunxing mingling",
    },
    StaticEntry {
        title_zh: b"\xe4\xbb\xbb\xe5\x8a\xa1\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\0",
        title_en: b"Task Manager\0",
        subtitle_zh: b"\xe6\x9f\xa5\xe7\x9c\x8b\xe8\xbf\x9b\xe7\xa8\x8b\xe5\x92\x8c\xe6\x80\xa7\xe8\x83\xbd\0",
        subtitle_en: b"View processes and performance\0",
        kind: EntryKind::App,
        path: b"/apps/system/taskmgr.elf",
        arg: b"",
        keywords: b"task manager process renwu jincheng xingneng",
    },
    StaticEntry {
        title_zh: b"\xe8\xae\xa1\xe7\xae\x97\xe5\x99\xa8\0",
        title_en: b"Calculator\0",
        subtitle_zh: b"/apps/builtin/calc.elf\0",
        subtitle_en: b"/apps/builtin/calc.elf\0",
        kind: EntryKind::App,
        path: b"/apps/builtin/calc.elf",
        arg: b"",
        keywords: b"calc calculator jisuanqi",
    },
    StaticEntry {
        title_zh: b"\xe6\x96\x87\xe6\x9c\xac\xe7\xbc\x96\xe8\xbe\x91\xe5\x99\xa8\0",
        title_en: b"Text Editor\0",
        subtitle_zh: b"/apps/builtin/texter.elf\0",
        subtitle_en: b"/apps/builtin/texter.elf\0",
        kind: EntryKind::App,
        path: b"/apps/builtin/texter.elf",
        arg: b"",
        keywords: b"text editor texter wenben bianji",
    },
    StaticEntry {
        title_zh: b"\xe5\x9b\xbe\xe7\x89\x87\xe6\x9f\xa5\xe7\x9c\x8b\xe5\x99\xa8\0",
        title_en: b"Image Viewer\0",
        subtitle_zh: b"/apps/builtin/picturer.elf\0",
        subtitle_en: b"/apps/builtin/picturer.elf\0",
        kind: EntryKind::App,
        path: b"/apps/builtin/picturer.elf",
        arg: b"",
        keywords: b"picture image viewer picturer tupian chakankan",
    },
    StaticEntry {
        title_zh: b"\xe6\xb5\x8f\xe8\xa7\x88\xe5\x99\xa8\0",
        title_en: b"Browser\0",
        subtitle_zh: b"/apps/builtin/browser.elf\0",
        subtitle_en: b"/apps/builtin/browser.elf\0",
        kind: EntryKind::App,
        path: b"/apps/builtin/browser.elf",
        arg: b"",
        keywords: b"browser web liulanqi wangye",
    },
    StaticEntry {
        title_zh: b"\xe7\xa3\x81\xe7\x9b\x98\xe4\xb8\x8e\xe5\x88\x86\xe5\x8c\xba\0",
        title_en: b"Disks & Partitions\0",
        subtitle_zh: b"\xe6\x89\x93\xe5\xbc\x80\xe6\x96\x87\xe4\xbb\xb6\xe8\xb5\x84\xe6\xba\x90\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\0",
        subtitle_en: b"Open File Manager\0",
        kind: EntryKind::Setting,
        path: b"/apps/system/fmanager.elf",
        arg: b"/",
        keywords: b"disk partition root cipan fenqu genmulu",
    },
];

static APP_DIRS: &[&[u8]] = &[b"/apps/system", b"/apps/builtin"];
static FILE_SEARCH_DIRS: &[&[u8]] = &[b"/system", b"/system/resources"];

static mut WINDOW_HANDLE: Hdl = 0;
static mut STATE: LauncherState = LauncherState::new();
static mut DIR_BUFFER: [DirNode; DIRNODE_COUNT] = [EMPTY_DIRNODE; DIRNODE_COUNT];
static mut LANGUAGE: c_int = rust_i18n::XJ380_LANGUAGE_ZH_CN;

fn zero(buf: &mut [u8]) {
    let mut i = 0;
    while i < buf.len() {
        buf[i] = 0;
        i += 1;
    }
}

fn copy_bytes(dst: &mut [u8], src: &[u8]) {
    if dst.is_empty() {
        return;
    }

    zero(dst);
    let mut i = 0;
    while i + 1 < dst.len() && i < src.len() && src[i] != 0 {
        dst[i] = src[i];
        i += 1;
    }
    dst[i] = 0;
}

fn cstr_len(buf: &[u8]) -> usize {
    let mut len = 0;
    while len < buf.len() && buf[len] != 0 {
        len += 1;
    }
    len
}

fn append_bytes_from(dst: &mut [u8], pos: &mut usize, src: &[u8]) {
    let mut i = 0;
    while *pos + 1 < dst.len() && i < src.len() && src[i] != 0 {
        dst[*pos] = src[i];
        *pos += 1;
        i += 1;
    }
    if *pos < dst.len() {
        dst[*pos] = 0;
    }
}

fn path_join(out: &mut [u8], base: &[u8], name: &[u8]) {
    zero(out);
    let mut pos = 0;
    append_bytes_from(out, &mut pos, base);
    if pos > 0 && out[pos - 1] != b'/' && pos + 1 < out.len() {
        out[pos] = b'/';
        pos += 1;
        out[pos] = 0;
    }
    append_bytes_from(out, &mut pos, name);
}

fn starts_with(buf: &[u8], prefix: &[u8]) -> bool {
    let mut i = 0;
    while i < prefix.len() {
        if i >= buf.len() || buf[i] != prefix[i] {
            return false;
        }
        i += 1;
    }
    true
}

fn has_suffix(buf: &[u8], suffix: &[u8]) -> bool {
    let len = cstr_len(buf);
    if suffix.len() > len {
        return false;
    }
    let start = len - suffix.len();
    let mut i = 0;
    while i < suffix.len() {
        if to_lower(buf[start + i]) != to_lower(suffix[i]) {
            return false;
        }
        i += 1;
    }
    true
}

fn to_lower(ch: u8) -> u8 {
    if ch >= b'A' && ch <= b'Z' {
        ch + 32
    } else {
        ch
    }
}

fn bytes_equal(a: &[u8], b: &[u8]) -> bool {
    let alen = cstr_len(a);
    let blen = cstr_len(b);
    if alen != blen {
        return false;
    }
    let mut i = 0;
    while i < alen {
        if a[i] != b[i] {
            return false;
        }
        i += 1;
    }
    true
}

fn bytes_equal_casefold(a: &[u8], b: &[u8]) -> bool {
    let alen = cstr_len(a);
    let blen = cstr_len(b);
    if alen != blen {
        return false;
    }
    let mut i = 0;
    while i < alen {
        if to_lower(a[i]) != to_lower(b[i]) {
            return false;
        }
        i += 1;
    }
    true
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

fn entry_title(entry: &StaticEntry) -> &'static [u8] {
    tr_bytes(entry.title_zh, entry.title_en)
}

fn entry_subtitle(entry: &StaticEntry) -> &'static [u8] {
    tr_bytes(entry.subtitle_zh, entry.subtitle_en)
}

fn starts_with_casefold(buf: &[u8], prefix: &[u8]) -> bool {
    let plen = cstr_len(prefix);
    if plen == 0 || plen > cstr_len(buf) {
        return false;
    }
    let mut i = 0;
    while i < plen {
        if to_lower(buf[i]) != to_lower(prefix[i]) {
            return false;
        }
        i += 1;
    }
    true
}

fn contains_casefold_pos(haystack: &[u8], needle: &[u8]) -> i32 {
    let nlen = cstr_len(needle);
    if nlen == 0 {
        return 0;
    }
    let hlen = cstr_len(haystack);
    if nlen > hlen {
        return -1;
    }

    let mut start = 0;
    while start + nlen <= hlen {
        let mut matched = true;
        let mut i = 0;
        while i < nlen {
            if to_lower(haystack[start + i]) != to_lower(needle[i]) {
                matched = false;
                break;
            }
            i += 1;
        }
        if matched {
            return start as i32;
        }
        start += 1;
    }
    -1
}

fn fuzzy_score(haystack: &[u8], query: &[u8]) -> i32 {
    let qlen = cstr_len(query);
    if qlen == 0 {
        return 1;
    }
    let hlen = cstr_len(haystack);
    if hlen == 0 {
        return -1;
    }
    if bytes_equal_casefold(haystack, query) {
        return 1000 - hlen as i32;
    }
    if starts_with_casefold(haystack, query) {
        return 850 - hlen as i32;
    }
    let pos = contains_casefold_pos(haystack, query);
    if pos >= 0 {
        return 700 - pos * 8 - (hlen as i32 / 2);
    }

    let mut qi = 0usize;
    let mut hi = 0usize;
    let mut last: i32 = -1;
    let mut gap: i32 = 0;
    while hi < hlen && qi < qlen {
        if to_lower(haystack[hi]) == to_lower(query[qi]) {
            if last >= 0 {
                gap += hi as i32 - last - 1;
            }
            last = hi as i32;
            qi += 1;
        }
        hi += 1;
    }
    if qi == qlen {
        return 420 - gap * 6 - hlen as i32 / 3;
    }
    -1
}

fn max_score(a: i32, b: i32) -> i32 {
    if a > b {
        a
    } else {
        b
    }
}

fn entry_match_score(entry: &StaticEntry, query: &[u8]) -> i32 {
    let mut score = fuzzy_score(entry.title_zh, query);
    score = max_score(score, fuzzy_score(entry.title_en, query));
    score = max_score(score, fuzzy_score(entry.subtitle_zh, query) - 30);
    score = max_score(score, fuzzy_score(entry.subtitle_en, query) - 30);
    score = max_score(score, fuzzy_score(entry.path, query) - 20);
    score = max_score(score, fuzzy_score(entry.keywords, query) + 30);
    score
}

fn file_match_score(label: &[u8], path: &[u8], query: &[u8]) -> i32 {
    max_score(fuzzy_score(label, query), fuzzy_score(path, query) - 25)
}

fn static_path_exists(path: &[u8]) -> bool {
    let mut i = 0;
    while i < ENTRIES.len() {
        if bytes_equal(ENTRIES[i].path, path) {
            return true;
        }
        i += 1;
    }
    false
}

fn recent_boost_for_path(state: &LauncherState, path: &[u8]) -> i32 {
    let mut i = 0;
    while i < state.recent_count {
        if bytes_equal(&state.recents[i].path, path) {
            return 520 - (i as i32) * 28;
        }
        i += 1;
    }
    0
}

fn item_path_equals(state: &LauncherState, item: EntryRef, path: &[u8]) -> bool {
    match item {
        EntryRef::Static(index) => bytes_equal(ENTRIES[index].path, path),
        EntryRef::File(index) => bytes_equal(&state.file_results[index].path, path),
        EntryRef::DirectPath => bytes_equal(&state.query, path),
    }
}

fn candidate_has_path(candidates: &[Candidate; MAX_CANDIDATES], count: usize, state: &LauncherState, path: &[u8]) -> bool {
    let mut i = 0;
    while i < count {
        if item_path_equals(state, candidates[i].item, path) {
            return true;
        }
        i += 1;
    }
    false
}

fn add_candidate(
    state: &LauncherState,
    candidates: &mut [Candidate; MAX_CANDIDATES],
    count: &mut usize,
    item: EntryRef,
    score: i32,
) {
    if *count >= MAX_CANDIDATES || score < 0 {
        return;
    }
    let path = match item {
        EntryRef::Static(index) => ENTRIES[index].path,
        EntryRef::File(index) => &state.file_results[index].path,
        EntryRef::DirectPath => &state.query,
    };
    if candidate_has_path(candidates, *count, state, path) {
        return;
    }
    candidates[*count] = Candidate { item, score };
    *count += 1;
}

fn find_file_result_by_path(state: &LauncherState, path: &[u8]) -> Option<usize> {
    let mut i = 0;
    while i < state.file_count {
        if bytes_equal(&state.file_results[i].path, path) {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn add_file_result(state: &mut LauncherState, label: &[u8], path: &[u8], filetype: u64, kind: EntryKind) -> Option<usize> {
    if let Some(index) = find_file_result_by_path(state, path) {
        return Some(index);
    }
    if state.file_count >= MAX_FILE_RESULTS {
        return None;
    }
    let index = state.file_count;
    state.file_results[index] = EMPTY_FILE_RESULT;
    copy_bytes(&mut state.file_results[index].label, label);
    copy_bytes(&mut state.file_results[index].path, path);
    state.file_results[index].filetype = filetype;
    state.file_results[index].kind = kind;
    state.file_count += 1;
    Some(index)
}

fn label_from_filename(out: &mut [u8], filename: &[u8]) {
    zero(out);
    let len = cstr_len(filename);
    let mut copy_len = len;
    if len > 4
        && (has_suffix(filename, b".elf") || has_suffix(filename, b".epf"))
    {
        copy_len = len - 4;
    }
    let mut i = 0;
    while i + 1 < out.len() && i < copy_len {
        out[i] = filename[i];
        i += 1;
    }
    if i < out.len() {
        out[i] = 0;
    }
}

fn is_executable_path(path: &[u8]) -> bool {
    has_suffix(path, b".elf") || has_suffix(path, b".epf")
}

fn scan_dir_candidates(
    state: &mut LauncherState,
    dir: &[u8],
    query: &[u8],
    only_apps: bool,
    candidates: &mut [Candidate; MAX_CANDIDATES],
    candidate_count: &mut usize,
) {
    let query_len = cstr_len(query);
    let mut path = [0u8; PATH_CAP];
    copy_bytes(&mut path, dir);
    let mut count: u32 = 0;

    unsafe {
        zero_dir_buffer();
        xapi_SearchFile(path.as_mut_ptr() as *mut c_char, &mut count as *mut u32, DIR_BUFFER.as_mut_ptr());
        if count == 404 {
            return;
        }
        if count > DIRNODE_COUNT as u32 {
            count = DIRNODE_COUNT as u32;
        }

        let mut i = 0usize;
        while i < count as usize && *candidate_count < MAX_CANDIDATES {
            let node = &DIR_BUFFER[i];
            let mut full_path = [0u8; PATH_CAP];
            path_join(&mut full_path, dir, &node.filename);

            let executable = is_executable_path(&full_path);
            if only_apps && !executable {
                i += 1;
                continue;
            }
            if executable && static_path_exists(&full_path) {
                i += 1;
                continue;
            }

            let mut label = [0u8; LABEL_CAP];
            if executable {
                label_from_filename(&mut label, &node.filename);
            } else {
                copy_bytes(&mut label, &node.filename);
            }

            let mut score = if query_len == 0 {
                if only_apps {
                    360 - i as i32
                } else {
                    -1
                }
            } else {
                file_match_score(&label, &full_path, query)
            };
            if score < 0 {
                i += 1;
                continue;
            }

            score += recent_boost_for_path(state, &full_path);
            let kind = if executable { EntryKind::App } else { EntryKind::File };
            if let Some(index) = add_file_result(state, &label, &full_path, node.filetype, kind) {
                add_candidate(state, candidates, candidate_count, EntryRef::File(index), score);
            }
            i += 1;
        }
    }
}

unsafe fn zero_dir_buffer() {
    let mut i = 0;
    while i < DIRNODE_COUNT {
        unsafe {
            DIR_BUFFER[i] = EMPTY_DIRNODE;
        }
        i += 1;
    }
}

fn current_user_base_path(out: &mut [u8]) {
    rust_i18n::current_user_base_path(out, b"/users/Root");
}

fn current_desktop_path(out: &mut [u8]) {
    let mut base = [0u8; PATH_CAP];
    current_user_base_path(&mut base);
    path_join(out, &base, b"desktop");
}

fn current_recent_path(out: &mut [u8]) {
    let mut base = [0u8; PATH_CAP];
    current_user_base_path(&mut base);
    path_join(out, &base, b"launcher_recent.dat");
}

fn add_loaded_recent(state: &mut LauncherState, path: &[u8], filetype: u64) {
    if path.is_empty() || path[0] != b'/' || state.recent_count >= MAX_RECENTS {
        return;
    }
    let mut i = 0;
    while i < state.recent_count {
        if bytes_equal(&state.recents[i].path, path) {
            return;
        }
        i += 1;
    }
    let index = state.recent_count;
    state.recents[index] = EMPTY_RECENT_ITEM;
    copy_bytes(&mut state.recents[index].path, path);
    state.recents[index].filetype = filetype;
    state.recent_count += 1;
}

fn parse_recent_line(state: &mut LauncherState, line: &[u8], len: usize) {
    if len == 0 {
        return;
    }
    let mut filetype = 0u64;
    let mut start = 0usize;
    if len > 2 && line[1] == b'|' {
        if line[0] == b'1' {
            filetype = 1;
        }
        start = 2;
    }
    if start >= len || line[start] != b'/' {
        return;
    }
    add_loaded_recent(state, &line[start..len], filetype);
}

fn load_recent_items(state: &mut LauncherState) {
    state.recents_loaded = true;
    state.recent_count = 0;
    let mut path = [0u8; PATH_CAP];
    current_recent_path(&mut path);

    unsafe {
        let file = xapi_OpenFile(path.as_mut_ptr() as *mut c_char);
        if file.is_null() {
            return;
        }
        let length = if (*file).length as usize > RECENT_FILE_BYTES {
            RECENT_FILE_BYTES
        } else {
            (*file).length as usize
        };
        let buffer = (*file).buffer;
        if !buffer.is_null() {
            let mut line = [0u8; PATH_CAP + 4];
            let mut line_len = 0usize;
            let mut i = 0usize;
            while i < length && state.recent_count < MAX_RECENTS {
                let ch = *buffer.add(i);
                if ch == 0 {
                    break;
                }
                if ch == b'\n' || ch == b'\r' {
                    parse_recent_line(state, &line, line_len);
                    zero(&mut line);
                    line_len = 0;
                } else if line_len + 1 < line.len() {
                    line[line_len] = ch;
                    line_len += 1;
                }
                i += 1;
            }
            if line_len > 0 && state.recent_count < MAX_RECENTS {
                parse_recent_line(state, &line, line_len);
            }
        }
        xapi_CloseFile(file);
    }
}

fn save_recent_items(state: &mut LauncherState) {
    let mut path = [0u8; PATH_CAP];
    current_recent_path(&mut path);

    let mut data = [0u8; RECENT_FILE_BYTES];
    let mut pos = 0usize;
    let mut i = 0usize;
    while i < state.recent_count {
        if pos + 4 >= data.len() {
            break;
        }
        data[pos] = if state.recents[i].filetype == 1 { b'1' } else { b'0' };
        pos += 1;
        data[pos] = b'|';
        pos += 1;
        append_bytes_from(&mut data, &mut pos, &state.recents[i].path);
        if pos + 1 < data.len() {
            data[pos] = b'\n';
            pos += 1;
            data[pos] = 0;
        }
        i += 1;
    }

    unsafe {
        xapi_WriteFile(
            path.as_mut_ptr() as *mut c_char,
            data.as_mut_ptr() as *mut c_char,
            data.len() as u64,
            0,
        );
    }
}

fn record_recent_path(path: &[u8], filetype: u64) {
    unsafe {
        if !STATE.recents_loaded {
            load_recent_items(&mut STATE);
        }
        if path.is_empty() || path[0] != b'/' {
            return;
        }

        let mut found = MAX_RECENTS;
        let mut i = 0usize;
        while i < STATE.recent_count {
            if bytes_equal(&STATE.recents[i].path, path) {
                found = i;
                break;
            }
            i += 1;
        }

        if found == 0 {
            STATE.recents[0].filetype = filetype;
            save_recent_items(&mut STATE);
            return;
        }

        let mut limit = STATE.recent_count;
        if found < MAX_RECENTS {
            limit = found;
        } else if STATE.recent_count < MAX_RECENTS {
            STATE.recent_count += 1;
        }

        let mut j = limit;
        while j > 0 {
            STATE.recents[j] = STATE.recents[j - 1];
            j -= 1;
        }
        STATE.recents[0] = EMPTY_RECENT_ITEM;
        copy_bytes(&mut STATE.recents[0].path, path);
        STATE.recents[0].filetype = filetype;
        save_recent_items(&mut STATE);
    }
}

fn add_recent_candidates(
    state: &mut LauncherState,
    query: &[u8],
    candidates: &mut [Candidate; MAX_CANDIDATES],
    candidate_count: &mut usize,
) {
    let query_len = cstr_len(query);
    let mut i = 0usize;
    while i < state.recent_count && *candidate_count < MAX_CANDIDATES {
        let recent = state.recents[i];
        if static_path_exists(&recent.path) {
            i += 1;
            continue;
        }

        let mut label = [0u8; LABEL_CAP];
        basename_from_path(&mut label, &recent.path);
        let mut score = if query_len == 0 {
            920 - i as i32 * 24
        } else {
            file_match_score(&label, &recent.path, query) + 320 - i as i32 * 12
        };
        if query_len != 0 && score < 0 {
            i += 1;
            continue;
        }
        if is_executable_path(&recent.path) {
            score += 40;
        }
        if let Some(index) = add_file_result(state, &label, &recent.path, recent.filetype, EntryKind::Recent) {
            add_candidate(state, candidates, candidate_count, EntryRef::File(index), score);
        }
        i += 1;
    }
}

fn basename_from_path(out: &mut [u8], path: &[u8]) {
    zero(out);
    let len = cstr_len(path);
    let mut start = 0usize;
    let mut i = 0usize;
    while i < len {
        if path[i] == b'/' {
            start = i + 1;
        }
        i += 1;
    }
    copy_bytes(out, &path[start..len]);
    if is_executable_path(out) {
        let out_len = cstr_len(out);
        if out_len > 4 {
            out[out_len - 4] = 0;
        }
    }
}

fn add_static_candidates(
    state: &LauncherState,
    query: &[u8],
    candidates: &mut [Candidate; MAX_CANDIDATES],
    candidate_count: &mut usize,
) {
    let query_len = cstr_len(query);
    let mut i = 0usize;
    while i < ENTRIES.len() && *candidate_count < MAX_CANDIDATES {
        let mut score = if query_len == 0 {
            520 - i as i32
        } else {
            entry_match_score(&ENTRIES[i], query)
        };
        if score >= 0 {
            score += recent_boost_for_path(state, ENTRIES[i].path);
            match ENTRIES[i].kind {
                EntryKind::Command => score += 50,
                EntryKind::Setting => score += 30,
                _ => {}
            }
            add_candidate(state, candidates, candidate_count, EntryRef::Static(i), score);
        }
        i += 1;
    }
}

fn commit_best_results(state: &mut LauncherState, candidates: &[Candidate; MAX_CANDIDATES], candidate_count: usize) {
    state.result_count = 0;
    let mut used = [false; MAX_CANDIDATES];
    while state.result_count < MAX_RESULTS {
        let mut best_index = MAX_CANDIDATES;
        let mut best_score = -1;
        let mut i = 0usize;
        while i < candidate_count {
            if !used[i] && candidates[i].score > best_score {
                best_score = candidates[i].score;
                best_index = i;
            }
            i += 1;
        }
        if best_index == MAX_CANDIDATES {
            break;
        }
        used[best_index] = true;
        state.results[state.result_count] = candidates[best_index].item;
        state.result_count += 1;
    }
}

fn build_results(state: &mut LauncherState) {
    if !state.recents_loaded {
        load_recent_items(state);
    }

    state.result_count = 0;
    state.file_count = 0;

    let query = state.query;
    let query_len = state.query_len;
    let mut candidates = [EMPTY_CANDIDATE; MAX_CANDIDATES];
    let mut candidate_count = 0usize;

    if query_len > 0 && is_direct_path(&query) {
        add_candidate(state, &mut candidates, &mut candidate_count, EntryRef::DirectPath, 980);
    }

    add_static_candidates(state, &query, &mut candidates, &mut candidate_count);
    add_recent_candidates(state, &query, &mut candidates, &mut candidate_count);

    let mut d = 0usize;
    while d < APP_DIRS.len() {
        scan_dir_candidates(state, APP_DIRS[d], &query, true, &mut candidates, &mut candidate_count);
        d += 1;
    }

    if query_len > 0 {
        let mut desktop = [0u8; PATH_CAP];
        current_desktop_path(&mut desktop);
        scan_dir_candidates(state, &desktop, &query, false, &mut candidates, &mut candidate_count);

        let mut f = 0usize;
        while f < FILE_SEARCH_DIRS.len() {
            scan_dir_candidates(state, FILE_SEARCH_DIRS[f], &query, false, &mut candidates, &mut candidate_count);
            f += 1;
        }
    }

    commit_best_results(state, &candidates, candidate_count);

    if state.result_count == 0 {
        copy_bytes(
            &mut state.status,
            tr_bytes(
                b"\xe6\xb2\xa1\xe6\x9c\x89\xe6\x89\xbe\xe5\x88\xb0\xe5\x8c\xb9\xe9\x85\x8d\xe9\xa1\xb9",
                b"No matches found",
            ),
        );
    } else if query_len == 0 {
        copy_bytes(
            &mut state.status,
            tr_bytes(
                b"\xe8\xbe\x93\xe5\x85\xa5\xe5\xba\x94\xe7\x94\xa8\xe3\x80\x81\xe6\x96\x87\xe4\xbb\xb6\xe3\x80\x81\xe8\xae\xbe\xe7\xbd\xae\xe6\x88\x96\xe5\x91\xbd\xe4\xbb\xa4",
                b"Type an app, file, setting, or command",
            ),
        );
    } else {
        copy_bytes(
            &mut state.status,
            tr_bytes(
                b"\xe6\x8c\x89 Enter \xe6\x89\x93\xe5\xbc\x80\xef\xbc\x8cEsc \xe5\x85\xb3\xe9\x97\xad",
                b"Press Enter to open, Esc to close",
            ),
        );
    }

    if state.selected >= state.result_count {
        state.selected = 0;
    }

}

fn is_direct_path(query: &[u8]) -> bool {
    if starts_with(query, b"/") {
        return true;
    }
    contains_casefold_pos(query, b".elf") >= 0 || contains_casefold_pos(query, b".epf") >= 0
}

fn result_title<'a>(state: &'a mut LauncherState, item: EntryRef) -> *mut c_char {
    match item {
        EntryRef::Static(index) => entry_title(&ENTRIES[index]).as_ptr() as *mut c_char,
        EntryRef::File(index) => state.file_results[index].label.as_mut_ptr() as *mut c_char,
        EntryRef::DirectPath => tr_cstr(
            c"\xe8\xbf\x90\xe8\xa1\x8c\xe8\xbe\x93\xe5\x85\xa5\xe7\x9a\x84\xe8\xb7\xaf\xe5\xbe\x84".as_ptr(),
            c"Run typed path".as_ptr(),
        ),
    }
}

fn result_subtitle<'a>(state: &'a mut LauncherState, item: EntryRef) -> *mut c_char {
    match item {
        EntryRef::Static(index) => entry_subtitle(&ENTRIES[index]).as_ptr() as *mut c_char,
        EntryRef::File(index) => state.file_results[index].path.as_mut_ptr() as *mut c_char,
        EntryRef::DirectPath => state.query.as_mut_ptr() as *mut c_char,
    }
}

fn kind_text(kind: EntryKind, filetype: u64) -> *mut c_char {
    match kind {
        EntryKind::App => tr_cstr(c"\xe5\xba\x94\xe7\x94\xa8".as_ptr(), c"App".as_ptr()),
        EntryKind::Setting => tr_cstr(c"\xe8\xae\xbe\xe7\xbd\xae".as_ptr(), c"Setting".as_ptr()),
        EntryKind::Command => tr_cstr(c"\xe5\x91\xbd\xe4\xbb\xa4".as_ptr(), c"Command".as_ptr()),
        EntryKind::Recent => tr_cstr(c"\xe6\x9c\x80\xe8\xbf\x91".as_ptr(), c"Recent".as_ptr()),
        EntryKind::File => {
            if filetype == 1 {
                tr_cstr(c"\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9".as_ptr(), c"Folder".as_ptr())
            } else {
                tr_cstr(c"\xe6\x96\x87\xe4\xbb\xb6".as_ptr(), c"File".as_ptr())
            }
        }
    }
}

fn result_kind_text(item: EntryRef, state: &LauncherState) -> *mut c_char {
    match item {
        EntryRef::Static(index) => kind_text(ENTRIES[index].kind, 0),
        EntryRef::File(index) => kind_text(state.file_results[index].kind, state.file_results[index].filetype),
        EntryRef::DirectPath => tr_cstr(c"\xe8\xb7\xaf\xe5\xbe\x84".as_ptr(), c"Path".as_ptr()),
    }
}

fn power_action_for_static(index: usize) -> u64 {
    let entry = ENTRIES[index];
    if bytes_equal(entry.path, b"power:reboot") {
        return XPOWER_REBOOT;
    }
    if bytes_equal(entry.path, b"power:shutdown") {
        return XPOWER_SHUTDOWN;
    }
    0
}

fn power_action_for_item(item: EntryRef) -> u64 {
    match item {
        EntryRef::Static(index) => power_action_for_static(index),
        _ => 0,
    }
}

fn result_icon(item: EntryRef, state: &LauncherState) -> *mut c_char {
    match item {
        EntryRef::Static(index) => match ENTRIES[index].kind {
            EntryKind::Setting => c"gear".as_ptr() as *mut c_char,
            EntryKind::Command => c"power-off".as_ptr() as *mut c_char,
            _ => c"file".as_ptr() as *mut c_char,
        },
        EntryRef::File(index) => {
            if state.file_results[index].filetype == 1 {
                c"folder-open".as_ptr() as *mut c_char
            } else if has_suffix(&state.file_results[index].path, b".png")
                || has_suffix(&state.file_results[index].path, b".jpg")
                || has_suffix(&state.file_results[index].path, b".bmp")
            {
                c"file-image".as_ptr() as *mut c_char
            } else if has_suffix(&state.file_results[index].path, b".txt")
                || has_suffix(&state.file_results[index].path, b".md")
            {
                c"file-lines".as_ptr() as *mut c_char
            } else if is_executable_path(&state.file_results[index].path) {
                c"file".as_ptr() as *mut c_char
            } else {
                c"file".as_ptr() as *mut c_char
            }
        }
        EntryRef::DirectPath => c"file".as_ptr() as *mut c_char,
    }
}

fn draw_window(handle: Hdl) {
    unsafe {
        build_results(&mut STATE);
        xapi_DrawRect(handle, 0, 0, WINDOW_WIDTH - 1, WINDOW_HEIGHT - 1, COLOR_BG, true);

        xapi_DrawRect(handle, 0, 0, WINDOW_WIDTH - 1, WINDOW_HEIGHT - 1, COLOR_LINE, false);
        xapi_DrawRect(handle, 1, 1, WINDOW_WIDTH - 2, 1, 0xffffffff, true);
        xapi_DrawRect(handle, 16, 16, WINDOW_WIDTH - 17, 66, COLOR_PANEL, true);
        xapi_DrawRect(handle, 16, 66, WINDOW_WIDTH - 17, 66, COLOR_LINE, true);
        xapi_DrawFA(handle, 34, 33, 16, c"file".as_ptr() as *mut c_char, false);
        if STATE.query_len == 0 {
            xapi_DrawText(
                handle,
                66,
                31,
                tr_cstr(
                    c"\xe8\xbe\x93\xe5\x85\xa5\xe5\xba\x94\xe7\x94\xa8\xe5\x90\x8d\xe3\x80\x81\xe6\x96\x87\xe4\xbb\xb6\xe5\x90\x8d\xe3\x80\x81\xe8\xae\xbe\xe7\xbd\xae\xe3\x80\x81\xe9\x87\x8d\xe5\x90\xaf".as_ptr(),
                    c"Search apps, files, settings, commands".as_ptr(),
                ),
                15,
                COLOR_MUTED,
            );
        } else {
            xapi_DrawText(handle, 66, 31, STATE.query.as_mut_ptr() as *mut c_char, 15, COLOR_TEXT);
        }

        let mut i = 0;
        while i < STATE.result_count {
            draw_result_row(handle, i);
            i += 1;
        }

        if STATE.result_count == 0 {
            xapi_DrawText(handle, 34, 110, STATE.status.as_mut_ptr() as *mut c_char, 12, COLOR_ERROR);
        } else {
            xapi_DrawText(handle, 28, WINDOW_HEIGHT - 30, STATE.status.as_mut_ptr() as *mut c_char, 10, COLOR_MUTED);
        }

        xapi_RefreshWindow(handle);
        STATE.redraw = false;
    }
}

fn draw_result_row(handle: Hdl, index: usize) {
    unsafe {
        let y = ROW_TOP + (index as i32) * ROW_HEIGHT;
        let selected = index == STATE.selected;
        let bg = if selected { COLOR_ROW_HOVER } else { COLOR_PANEL };
        xapi_DrawRect(handle, 16, y as u32, WINDOW_WIDTH - 17, (y + ROW_HEIGHT - 2) as u32, bg, true);
        if selected {
            xapi_DrawRect(handle, 16, y as u32, 20, (y + ROW_HEIGHT - 2) as u32, COLOR_ACCENT, true);
        }

        let item = STATE.results[index];
        xapi_DrawFA(handle, 38, (y + 7) as u32, 13, result_icon(item, &STATE), false);
        xapi_DrawText(handle, 64, (y + 5) as u32, result_title(&mut STATE, item), 11, COLOR_TEXT);
        xapi_DrawText(handle, 250, (y + 6) as u32, result_subtitle(&mut STATE, item), 10, COLOR_MUTED);
        xapi_DrawText(handle, 632, (y + 6) as u32, result_kind_text(item, &STATE), 10, COLOR_MUTED);
        xapi_DrawRect(handle, 28, (y + ROW_HEIGHT - 1) as u32, WINDOW_WIDTH - 28, (y + ROW_HEIGHT - 1) as u32, COLOR_LINE, true);
    }
}

fn query_push(ch: u8) {
    unsafe {
        if STATE.query_len + 1 >= MAX_QUERY {
            return;
        }
        STATE.query[STATE.query_len] = ch;
        STATE.query_len += 1;
        STATE.query[STATE.query_len] = 0;
        STATE.selected = 0;
        STATE.redraw = true;
    }
}

fn query_backspace() {
    unsafe {
        if STATE.query_len == 0 {
            return;
        }
        STATE.query_len -= 1;
        STATE.query[STATE.query_len] = 0;
        STATE.selected = 0;
        STATE.redraw = true;
    }
}

fn select_next() {
    unsafe {
        if STATE.result_count == 0 {
            return;
        }
        if STATE.selected + 1 < STATE.result_count {
            STATE.selected += 1;
        } else {
            STATE.selected = 0;
        }
        STATE.redraw = true;
    }
}

fn select_prev() {
    unsafe {
        if STATE.result_count == 0 {
            return;
        }
        if STATE.selected == 0 {
            STATE.selected = STATE.result_count - 1;
        } else {
            STATE.selected -= 1;
        }
        STATE.redraw = true;
    }
}

fn close_launcher() {
    unsafe {
        STATE.exit = true;
        xapi_CloseWindow(WINDOW_HANDLE);
    }
}

fn run_with_optional_arg(path: &mut [u8], arg: &mut [u8]) -> bool {
    unsafe {
        if cstr_len(arg) == 0 {
            xapi_Run(path.as_mut_ptr() as *mut c_char);
            return true;
        }
        let mut argv = [arg.as_mut_ptr() as *mut c_char, core::ptr::null_mut()];
        xapi_RunArgs(path.as_mut_ptr() as *mut c_char, argv.as_mut_ptr()) > 0
    }
}

fn run_static(index: usize) -> bool {
    let entry = ENTRIES[index];

    let mut path = [0u8; PATH_CAP];
    let mut arg = [0u8; PATH_CAP];
    copy_bytes(&mut path, entry.path);
    copy_bytes(&mut arg, entry.arg);
    let ok = run_with_optional_arg(&mut path, &mut arg);
    if ok {
        record_recent_path(entry.path, 0);
    }
    ok
}

fn run_file(index: usize) -> bool {
    unsafe {
        let result = STATE.file_results[index];
        let mut path = result.path;
        let mut arg = result.path;
        let ok = if result.filetype == 1 {
            let mut fmanager = [0u8; PATH_CAP];
            copy_bytes(&mut fmanager, b"/apps/system/fmanager.elf");
            run_with_optional_arg(&mut fmanager, &mut arg)
        } else if is_executable_path(&path) {
            xapi_Run(path.as_mut_ptr() as *mut c_char);
            true
        } else if has_suffix(&path, b".png")
            || has_suffix(&path, b".jpg")
            || has_suffix(&path, b".jpeg")
            || has_suffix(&path, b".bmp")
        {
            let mut viewer = [0u8; PATH_CAP];
            copy_bytes(&mut viewer, b"/apps/builtin/picturer.elf");
            run_with_optional_arg(&mut viewer, &mut arg)
        } else {
            let mut editor = [0u8; PATH_CAP];
            copy_bytes(&mut editor, b"/apps/builtin/texter.elf");
            run_with_optional_arg(&mut editor, &mut arg)
        };

        if ok {
            record_recent_path(&result.path, result.filetype);
        }
        ok
    }
}

fn run_direct_path() -> bool {
    unsafe {
        if STATE.query_len == 0 {
            return false;
        }
        let mut path = [0u8; PATH_CAP];
        copy_bytes(&mut path, &STATE.query);
        xapi_Run(path.as_mut_ptr() as *mut c_char);
        record_recent_path(&path, 0);
        true
    }
}

fn run_selected() {
    unsafe {
        if STATE.result_count == 0 {
            return;
        }
        let item = STATE.results[STATE.selected];
        let power_action = power_action_for_item(item);
        if power_action != 0 {
            xapi_PowerAction(power_action);
            close_launcher();
            return;
        }

        let ok = match item {
            EntryRef::Static(index) => run_static(index),
            EntryRef::File(index) => run_file(index),
            EntryRef::DirectPath => run_direct_path(),
        };
        if ok {
            close_launcher();
        } else {
            copy_bytes(
                &mut STATE.status,
                tr_bytes(
                    b"\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe6\xa3\x80\xe6\x9f\xa5\xe8\xb7\xaf\xe5\xbe\x84",
                    b"Launch failed, check the path",
                ),
            );
            STATE.redraw = true;
        }
    }
}

fn row_at(x: i32, y: i32) -> i32 {
    if x < 16 || x > 703 {
        return -1;
    }
    if y < ROW_TOP {
        return -1;
    }
    let index = (y - ROW_TOP) / ROW_HEIGHT;
    unsafe {
        if index < 0 || index as usize >= STATE.result_count {
            -1
        } else {
            index
        }
    }
}

extern "C" fn message_handler(msg_type: u64, h_data: u64, l_data: u64) {
    match msg_type {
        MSG_CHAR => {
            let ch = l_data as u8;
            if ch >= 32 && ch < 127 {
                query_push(ch);
            }
        }
        MSG_SPCHAR => {
            if l_data == b'\x08' as u64 {
                query_backspace();
            } else if l_data == b'\n' as u64 {
                run_selected();
            }
        }
        MSG_KEYDOWN => {
            if l_data == XKEY_ESC {
                close_launcher();
            } else if l_data == XKEY_ENTER || l_data == b'\n' as u64 {
                run_selected();
            } else if l_data == XKEY_DOWN {
                select_next();
            } else if l_data == XKEY_UP {
                select_prev();
            }
        }
        MSG_LBUTTON => {
            let row = row_at(h_data as i32, l_data as i32);
            if row >= 0 {
                unsafe {
                    STATE.selected = row as usize;
                    STATE.redraw = true;
                }
                run_selected();
            }
        }
        MSG_CRL => {}
        _ => {}
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn main(_argc: c_int, _argv: *mut *mut c_char, _envp: *mut *mut c_char) -> c_int {
    unsafe {
        STATE = LauncherState::new();
        LANGUAGE = rust_i18n::read_language();
        load_recent_items(&mut STATE);
        let mut window = XWindow {
            width: WINDOW_WIDTH,
            height: WINDOW_HEIGHT,
            title: tr_cstr(c"\xe5\x85\xa8\xe5\xb1\x80\xe6\x90\x9c\xe7\xb4\xa2".as_ptr(), c"Search".as_ptr()),
            sets: XWIN_FRAME_OFF,
        };

        xapi_CreateWindow(&raw mut WINDOW_HANDLE, &mut window);
        xapi_SetIcon(WINDOW_HANDLE, c"/system/icon/settings.png".as_ptr() as *mut c_char);
        SetMsgPrcor(WINDOW_HANDLE, message_handler);
        draw_window(WINDOW_HANDLE);

        loop {
            if STATE.exit {
                xapi_Exit(0);
            }
            if STATE.redraw {
                draw_window(WINDOW_HANDLE);
            }
            xapi_Sleep(8);
        }
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
