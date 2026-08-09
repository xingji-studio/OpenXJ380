# USERSPACE KNOWLEDGE BASE

## OVERVIEW
The user-space tree is intentionally minimal. It contains the XAPI runtime/API headers and one command-line example; GUI, browser, networking, terminal, and standalone test applications were removed on 2026-08-04 because those implementations were not mature enough.

## STRUCTURE
```
user/
├── xapi/          # Startup, syscall/libc wrappers, and public API headers
└── cli_shell.cpp  # The sole user-space command-line example
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Build targets | `tools/gen_ninja.py` | `user_apps()` emits only `shell.elf`. |
| Runtime entry | `xapi/arch/x86_64/crt0.S` | `_start` enters `xapi_start`. |
| Runtime wrappers | `xapi/*.cpp` | Syscall and libc/POSIX-like support. |
| Public APIs | `xapi/include/` | User/kernel ABI declarations. |
| Example program | `cli_shell.cpp` | Links against XAPI and is staged as `/apps/system/shell.elf`. |

## CONVENTIONS
- The generated Ninja graph builds XAPI objects before the example program.
- User apps link at `-Ttext=0x200000` and use the generated XAPI runtime objects.
- `main(int argc, char *argv[], char *envp[])` is reached through the XAPI startup shim.

## ANTI-PATTERNS
- Do not reintroduce removed GUI/browser/application trees without updating the project scope and build graph.
- Do not add a user app without updating `tools/gen_ninja.py` and image staging.
- Do not bypass XAPI startup objects unless a deliberate ABI change is being made.

## VERIFY
Use `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja build_xapi all` for user build coverage. Runtime behavior requires QEMU.
