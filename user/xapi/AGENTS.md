# XAPI KNOWLEDGE BASE

## OVERVIEW
XAPI is the user-space runtime/API layer. It produces the process entry shim, syscall/libc/POSIX-like wrappers, and public headers consumed by the command-line example.

## STRUCTURE
```
user/xapi/
├── arch/x86_64/crt0.S  # `_start` -> `xapi_start`
├── constart.cpp        # Console startup shim
├── libsys.cpp          # Syscall and runtime support
├── xgui_stubs.cpp      # Inert compatibility definitions for legacy declarations
└── include/            # User-facing XAPI/libsys/libc-like headers
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Build runtime | `tools/gen_ninja.py` XAPI section | Runtime objects are built before `shell.elf`. |
| ABI entry | `arch/x86_64/crt0.S` | Low-level `_start` trampoline. |
| Console startup | `constart.cpp` | Calls the app `main` and exits through XAPI. |
| Syscall/runtime support | `libsys.cpp` | User-facing kernel service wrappers. |
| Headers | `include/` | User/kernel API and ABI declarations. |

## CONVENTIONS
- Headers here are part of the user/kernel contract; coordinate changes with the corresponding kernel syscall handlers.
- The runtime is freestanding and only implements the subset required by current user-space code.
- GUI declarations are retained only for API/source compatibility; no GUI backend or GUI application is built.

## ANTI-PATTERNS
- Do not change syscall wrapper signatures in XAPI only; update kernel handlers and user headers together.
- Do not assume hosted libc behavior.
- Do not add startup objects without updating every app link rule in `tools/gen_ninja.py`.

## VERIFY
Build with `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja build_xapi all`. Runtime ABI changes require exercising the CLI example in QEMU.
