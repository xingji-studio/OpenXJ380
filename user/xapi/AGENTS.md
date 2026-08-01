# XAPI KNOWLEDGE BASE

## OVERVIEW
User-space runtime/API layer. Produces startup objects and libc/POSIX-like wrappers consumed by almost every user app.

## STRUCTURE
```
user/xapi/
├── start.cpp           # Generic GUI/app startup
├── constart.cpp        # Console startup wrapper
├── arch/x86_64/crt0.S  # `_start` -> `xapi_start`
├── include/            # User-facing xapi/libsys/libc-ish headers
└── *.cpp               # Runtime/syscall/libc/POSIX wrappers
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Build runtime | `tools/gen_ninja.py` XAPI section | Runtime objects plus GUI/console startup shims. |
| ABI entry | `arch/x86_64/crt0.S` | Low-level `_start` trampoline. |
| GUI startup | `start.cpp` | Sets `environ`, calls weak `main`. |
| Console startup | `constart.cpp` | Shell/terminal synchronization. |
| Headers | `include/xapi.h`, `include/x3api.h`, `include/libsys.h` | App-facing API. |
| libc-ish code | `krlibc.cpp`, `stdlib.cpp`, `stdio.cpp` | Runtime support. |

## CONVENTIONS
- The generated Ninja graph builds ordinary runtime objects plus `start.cpp.o` and `constart.cpp.o`, then copies `liballoc-x86_64.a` into `out/xapi`.
- `start.cpp` and `constart.cpp` expose weak C `main` and legacy C++ mangled main fallback.
- Both startup paths set `environ = envp` before app code runs.
- `constart.cpp` forks and execs `/apps/system/shell.elf` to establish terminal mode, then waits for `SXAH_CHECK_TERMINAL_INIT_STATUS`.
- Headers here are part of the user/kernel contract; coordinate changes with `kernel/syscall/` and `include/syscall/`.
- Assembly startup marks `.note.GNU-stack`; keep ABI minimal and architecture-specific.

## ANTI-PATTERNS
- Do not delete or rename startup objects without updating every app link rule in `tools/gen_ninja.py`.
- Do not change syscall wrapper signatures in xapi only; update kernel handlers and user headers together.
- Do not assume hosted libc behavior; this runtime is freestanding and partial.
- Do not ignore existing TODOs in `krlibc.cpp` formatting/error paths when changing printf-like code.

## VERIFY
Build with `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja build_xapi all`. Runtime ABI changes require running representative apps: shell, GUI app, signal/libc/POSIX demos.
