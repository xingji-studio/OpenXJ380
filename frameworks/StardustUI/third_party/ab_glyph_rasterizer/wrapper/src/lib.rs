use ab_glyph_rasterizer::{point, Rasterizer};
use std::ffi::c_float;
use std::os::raw::{c_int, c_uint};

#[repr(C)]
pub struct AbGlyphPoint {
    pub x: c_float,
    pub y: c_float,
}

#[repr(C)]
pub struct AbGlyphRasterizerHandle {
    inner: Rasterizer,
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_create(width: c_uint, height: c_uint) -> *mut AbGlyphRasterizerHandle {
    let handle = AbGlyphRasterizerHandle {
        inner: Rasterizer::new(width as usize, height as usize),
    };
    Box::into_raw(Box::new(handle))
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_destroy(handle: *mut AbGlyphRasterizerHandle) {
    if handle.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(handle));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_reset(handle: *mut AbGlyphRasterizerHandle, width: c_uint, height: c_uint) -> c_int {
    let Some(handle) = (unsafe { handle.as_mut() }) else {
        return 0;
    };
    handle.inner.reset(width as usize, height as usize);
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_clear(handle: *mut AbGlyphRasterizerHandle) -> c_int {
    let Some(handle) = (unsafe { handle.as_mut() }) else {
        return 0;
    };
    handle.inner.clear();
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_dimensions(
    handle: *const AbGlyphRasterizerHandle,
    out_width: *mut c_uint,
    out_height: *mut c_uint,
) -> c_int {
    let Some(handle) = (unsafe { handle.as_ref() }) else {
        return 0;
    };
    let (width, height) = handle.inner.dimensions();
    unsafe {
        if !out_width.is_null() {
            *out_width = width as c_uint;
        }
        if !out_height.is_null() {
            *out_height = height as c_uint;
        }
    }
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_draw_line(
    handle: *mut AbGlyphRasterizerHandle,
    p0: AbGlyphPoint,
    p1: AbGlyphPoint,
) -> c_int {
    let Some(handle) = (unsafe { handle.as_mut() }) else {
        return 0;
    };
    handle.inner.draw_line(point(p0.x, p0.y), point(p1.x, p1.y));
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_draw_quad(
    handle: *mut AbGlyphRasterizerHandle,
    p0: AbGlyphPoint,
    p1: AbGlyphPoint,
    p2: AbGlyphPoint,
) -> c_int {
    let Some(handle) = (unsafe { handle.as_mut() }) else {
        return 0;
    };
    handle.inner.draw_quad(point(p0.x, p0.y), point(p1.x, p1.y), point(p2.x, p2.y));
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_draw_cubic(
    handle: *mut AbGlyphRasterizerHandle,
    p0: AbGlyphPoint,
    p1: AbGlyphPoint,
    p2: AbGlyphPoint,
    p3: AbGlyphPoint,
) -> c_int {
    let Some(handle) = (unsafe { handle.as_mut() }) else {
        return 0;
    };
    handle
        .inner
        .draw_cubic(point(p0.x, p0.y), point(p1.x, p1.y), point(p2.x, p2.y), point(p3.x, p3.y));
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn stardustui_abgr_write_u8_alpha(
    handle: *const AbGlyphRasterizerHandle,
    out_pixels: *mut u8,
    out_len: c_uint,
) -> c_int {
    let Some(handle) = (unsafe { handle.as_ref() }) else {
        return 0;
    };
    if out_pixels.is_null() {
        return 0;
    }

    let mut wrote = 0usize;
    let max_len = out_len as usize;
    handle.inner.for_each_pixel(|index, alpha| {
        if index >= max_len {
            return;
        }
        let value = if alpha <= 0.0 {
            0u8
        } else if alpha >= 1.0 {
            255u8
        } else {
            (alpha * 255.0) as u8
        };
        unsafe {
            *out_pixels.add(index) = value;
        }
        wrote = index + 1;
    });

    if wrote == 0 && max_len == 0 {
        return 1;
    }
    if wrote > max_len {
        return 0;
    }
    1
}
