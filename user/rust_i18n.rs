#![allow(dead_code)]

use core::ffi::{c_char, c_int};

pub const XJ380_LANGUAGE_ZH_CN: c_int = 0;
pub const XJ380_LANGUAGE_EN_US: c_int = 1;

#[repr(C)]
struct XFile {
    length: u64,
    buffer: *mut u8,
}

#[repr(C)]
struct UserInfo {
    name: [c_char; 64],
    user_type: c_int,
}

#[repr(C)]
struct SettingsDataFileFormat {
    background_file_path: [u8; 256],
    clock_hour_offset: c_int,
    language: c_int,
}

unsafe extern "C" {
    fn xapi_GetCurrentUser(user_info: *mut UserInfo);
    fn xapi_OpenFile(path: *mut c_char) -> *mut XFile;
    fn xapi_CloseFile(file: *mut XFile);
}

pub fn normalize_language(language: c_int) -> c_int {
    if language == XJ380_LANGUAGE_EN_US {
        XJ380_LANGUAGE_EN_US
    } else {
        XJ380_LANGUAGE_ZH_CN
    }
}

fn clear(buf: &mut [u8]) {
    let mut i = 0usize;
    while i < buf.len() {
        buf[i] = 0;
        i += 1;
    }
}

fn append_bytes(dst: &mut [u8], pos: &mut usize, src: &[u8]) {
    let mut i = 0usize;
    while *pos + 1 < dst.len() && i < src.len() && src[i] != 0 {
        dst[*pos] = src[i];
        *pos += 1;
        i += 1;
    }
    if *pos < dst.len() {
        dst[*pos] = 0;
    }
}

fn append_user_name(dst: &mut [u8], pos: &mut usize, name: &[c_char; 64]) {
    let mut i = 0usize;
    while *pos + 1 < dst.len() && i < name.len() {
        let ch = name[i] as u8;
        if ch == 0 {
            break;
        }
        dst[*pos] = ch;
        *pos += 1;
        i += 1;
    }
    if *pos < dst.len() {
        dst[*pos] = 0;
    }
}

pub fn current_user_base_path(out: &mut [u8], fallback: &[u8]) -> bool {
    let mut user = UserInfo {
        name: [0; 64],
        user_type: 0,
    };
    unsafe {
        xapi_GetCurrentUser(&mut user);
    }
    if user.name[0] == 0 {
        clear(out);
        let mut pos = 0usize;
        append_bytes(out, &mut pos, fallback);
        return false;
    }

    clear(out);
    let mut pos = 0usize;
    append_bytes(out, &mut pos, b"/users/");
    append_user_name(out, &mut pos, &user.name);
    true
}

fn settings_path(out: &mut [u8]) -> bool {
    if !current_user_base_path(out, b"") {
        return false;
    }

    let mut pos = 0usize;
    while pos < out.len() && out[pos] != 0 {
        pos += 1;
    }
    append_bytes(out, &mut pos, b"/settings.dat");
    true
}

pub fn read_language() -> c_int {
    let mut language = XJ380_LANGUAGE_ZH_CN;
    let mut path = [0u8; 256];
    if !settings_path(&mut path) {
        return language;
    }

    unsafe {
        let file = xapi_OpenFile(path.as_mut_ptr() as *mut c_char);
        if file.is_null() {
            return language;
        }

        if !(*file).buffer.is_null()
            && (*file).length as usize >= core::mem::size_of::<SettingsDataFileFormat>()
        {
            let settings = (*file).buffer as *const SettingsDataFileFormat;
            language = (*settings).language;
        }

        xapi_CloseFile(file);
    }

    normalize_language(language)
}

pub fn tr_bytes_lang(language: c_int, zh_cn: &'static [u8], en_us: &'static [u8]) -> &'static [u8] {
    if normalize_language(language) == XJ380_LANGUAGE_EN_US {
        en_us
    } else {
        zh_cn
    }
}

pub fn tr_cstr_lang(language: c_int, zh_cn: *const c_char, en_us: *const c_char) -> *mut c_char {
    if normalize_language(language) == XJ380_LANGUAGE_EN_US {
        en_us as *mut c_char
    } else {
        zh_cn as *mut c_char
    }
}
