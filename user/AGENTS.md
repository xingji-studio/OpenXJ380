# USERSPACE KNOWLEDGE BASE

## OVERVIEW
User-space subtree: ELF apps, xapi runtime, browser/http/TLS integrations, terminal/shell, demos/tests, and app-specific resources.

## STRUCTURE
```
user/
├── xapi/          # Runtime/startup/POSIX-like API objects
├── include/       # App-side headers and mbed_compat
├── browser/       # Browser app and rendering/fetch/image glue
├── shell/         # System shell app
├── fmanager/, ctrlmenu/, taskmgr/, calc/, texter/, picturer/
└── *.cpp          # Single-file apps/demos/tests
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| App targets | `tools/gen_ninja.py` | Lists every `.elf`/host target. |
| Runtime dependency | `tools/gen_ninja.py` XAPI section | Must build before user apps. |
| GUI app startup | `xapi/start.cpp` | Generic `xapi_start` path. |
| Console app startup | `xapi/constart.cpp` | Forks/execs shell wrapper. |
| Browser/TLS | `browser/`, `https_client.cpp`, `http_tls_compat.cpp` | mbedtls/litehtml/libwebp integration. |
| Manual validation apps | `sigtest.cpp`, `posixdemo.cpp`, `libctest.cpp`, `fbtest.cpp`, `ccompat_demo.c` | Build-time QA surfaces. |

## CONVENTIONS
- The generated Ninja graph builds XAPI runtime objects before user apps; app links depend on `out/xapi/*.o`.
- User apps link at `-Ttext=0x200000` and generally include `out/xapi/liballoc-x86_64.a`.
- GUI-style apps exclude `constart.cpp.o`; console-style apps usually include `constart.cpp.o` and exclude `start.cpp.o`.
- `main(int argc, char *argv[], char *envp[])` is reached through xapi startup, not directly from the loader.
- Browser/httpget/nut pull Mbed TLS objects plus clang builtins; set `BROWSER_BUILTINS` if autodetection fails.
- `dyn-hello` is host-built, unlike normal freestanding `.elf` apps.
- Image staging copies selected apps into `/apps/system`, `/apps/builtin`, or `/apps` through `tools/ninja_build.py`.

## ANTI-PATTERNS
- Do not add a user app without updating `tools/gen_ninja.py` and image staging if it must appear in the image.
- Do not bypass xapi startup objects unless the app has a deliberate custom startup like browser.
- Do not edit vendored third-party trees for app behavior; keep integration in `user/`, `user/browser/`, or config wrappers.
- Do not assume `ninja check` runs user apps; it is static analysis only.

## VERIFY
Use `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja all` for user build coverage, then `ninja -f build.ninja vdisk` when image placement matters. Runtime behavior needs QEMU.
