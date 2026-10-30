# XAPI KNOWLEDGE BASE

## OVERVIEW
XAPI is the user-space runtime/API layer. It produces the process entry shim, syscall/libc/POSIX-like wrappers, and public headers consumed by the command-line example.

## STRUCTURE
```
user/xapi/
├── arch/x86_64/crt0.S  # `_start` -> `xapi_start`
├── constart.cpp        # Console startup shim
├── libsys.cpp          # Syscall and runtime support
├── libc_string.cpp     # String and memory functions (memcpy, strlen, strcmp, etc.)
├── libc_stdio.cpp      # Standard I/O (printf, snprintf, vfprintf, etc.)
├── libc_stdlib.cpp     # Standard library (atoi, strtol, qsort, bsearch, malloc)
├── libc_unistd.cpp     # POSIX syscall wrappers (read, write, open, fork, exec)
├── libc_misc.cpp       # ctype, strerror, perror, and malloc/free/realloc/calloc
├── test_libc.cpp       # libc self-test (42 checks)
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
| libc string/memory | `libc_string.cpp` | memcpy, memmove, memset, strlen, strcmp, strcpy, strstr, strdup, etc. |
| libc stdio | `libc_stdio.cpp` | printf/sprintf/snprintf/vfprintf — full format specifier support. |
| libc stdlib | `libc_stdlib.cpp` | atoi, strtol, strtoul, qsort (bubble sort), bsearch, getenv stub. |
| libc unistd | `libc_unistd.cpp` | open/read/write/close, fork/execve/waitpid, pipe/dup/sleep/chdir/stat. |
| libc misc | `libc_misc.cpp` | ctype, strerror, perror, malloc/free/calloc/realloc (bump allocator). |
| libc tests | `test_libc.cpp` | 42 checks covering printf, string, memory, stdlib. |
| Headers | `include/` | User/kernel API and ABI declarations. |

## CONVENTIONS
- Headers here are part of the user/kernel contract; coordinate changes with the corresponding kernel syscall handlers.
- The runtime is freestanding and only implements the subset required by current user-space code.
- GUI declarations are retained only for API/source compatibility; no GUI backend or GUI application is built.

## ANTI-PATTERNS
- Do not change syscall wrapper signatures in XAPI only; update kernel handlers and user headers together.
- Do not assume hosted libc behavior.
- Do not add startup objects without updating every app link rule in `tools/gen_ninja.py`.

## LIBC STATUS
### Implemented
| Module | Functions |
|--------|-----------|
| string | memcpy, memmove, memset, memchr, memcmp, bcmp, bcopy, bzero, strlen, strnlen, strcmp, strncmp, strcasecmp, strncasecmp, strcpy, strncpy, strcat, strchr, strchrnul, strrchr, strstr, strdup, strndup, strtok, strtok_r, strspn, strcspn, strpbrk, index, rindex |
| stdio | printf, fprintf, sprintf, snprintf, vprintf, vfprintf, vdprintf, vsprintf, vsnprintf. Supports %d/%i/%u/%x/%X/%o/%s/%c/%p/%%, flags (-0+# ), width (*), precision, length modifiers (hh/h/l/ll/z) |
| stdlib | atoi, atol, atoll, abs, strtol, strtoul, strtoll, strtoull, qsort, bsearch, getenv (stub), setenv (stub), unsetenv (stub) |
| unistd | read, write, open, close, lseek, mkdir, rmdir, unlink, rename, access, stat, fstat, lstat, getpid, getppid, getuid, geteuid, getgid, getegid, chdir, getcwd, dup, dup2, pipe, sleep, usleep, execve, execv, execvp, fork, vfork, wait, waitpid, exit, _exit, abort, isatty, fcntl, ioctl, getdents |
| ctype | isalpha, isdigit, isalnum, isspace, isupper, islower, toupper, tolower, isprint, ispunct, iscntrl, isxdigit, isgraph, isblank |
| malloc | malloc, free, calloc, realloc. Based on SYS_BRK, bump allocator, no coalescing. |
| errno | errno, strerror, perror |

### Not implemented
- FILE stream I/O (fopen, fclose, fread, fwrite, fgets, fseek, feof — only stdin/stdout/stderr)
- Signal handling (signal, sigaction, kill)
- Time functions (time, clock_gettime, gettimeofday)
- Network sockets (rely on XAPI wrappers instead)
- dirent wrappers (opendir, readdir — use raw getdents)
- rand/srand, div, ldiv, atexit, system
- strcoll, strxfrm, locale

### Known limitations
- `malloc` is a bump allocator. free() only marks blocks as freed; memory is not reused until the next brk extension. Fine for short-lived processes, not for long-running daemons.
- `qsort` uses bubble sort. The swap buffer is 256 bytes; elements larger than that will not be swapped correctly.
- `errno` is a single global variable, not thread-local.
- `printf` formats with size > 1024 characters allocate a heap buffer. If malloc fails, the output is silently truncated.
- `execvp` does not search PATH; it calls execv directly.
- No floating-point format specifiers (%f, %e, %g) are supported in printf.

## VERIFY
Build with `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja build_xapi all`. Runtime ABI changes require exercising the CLI example in QEMU.
