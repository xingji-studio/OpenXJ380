# GRAPHICS KNOWLEDGE BASE

## OVERVIEW
Built-in graphics/windowing subtree linked into `kernel.elf`. User apps reach this through xapi/syscalls; graphics code itself runs in kernel context.

## STRUCTURE
```
graphics/
├── image/          # stb image/resize and image decoders
├── logo/           # Built into kernel/image resources
├── window/         # Window manager/theme primitives
├── desktop.cpp     # Desktop surface behavior
├── sheet.cpp       # Sheet/layer manager
└── svg.cpp         # SVG parsing/rendering support
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Framebuffer/GOP | `include/graphics/GOP.hpp`, `driver/fbdev.cpp` | Early and runtime framebuffer access. |
| Sheets/layers | `sheet.cpp`, `include/graphics/sheet.h` | Desktop/window composition. |
| Window manager | `window/`, `include/graphics/window.h` | Theme/window primitives. |
| Image loading | `image/`, `svg.cpp` | Built-in decoders and renderer helpers. |
| User bridge | `kernel/syscall/xapi/xgui.cpp`, `user/xapi/include/` | User GUI API boundary. |

## CONVENTIONS
- Graphics is built into the kernel, not a user-space library.
- Image/logo assets can be embedded into kernel objects via `objcopy` in `tools/gen_ninja.py` or copied into `/system` by `ninja vdisk`.
- Many calls assume kernel heap/framebuffer state and scheduler timing from `KernelMain`; avoid moving early boot graphics into late UI code or vice versa.
- User GUI behavior must be surfaced through xapi/syscalls, not direct struct sharing with user apps.
- Visual behavior needs real QEMU/browser-style observation; build success is not enough.

## ANTI-PATTERNS
- Do not feed untrusted font data into `font/ttf/stb_ttf.h`; it explicitly has no security guarantee.
- Do not apply broad formatting or cleanup to vendored stb headers unless intentionally updating them.
- Do not bypass damage/refresh queues when existing code uses them for screen updates.

## VERIFY
`python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja all` plus QEMU visual validation. For user-visible GUI changes, boot image and exercise the desktop/app path, not just static checks.
