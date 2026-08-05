# KERNEL SYSCALL KNOWLEDGE BASE

## OVERVIEW
Kernel/user boundary implementation. This subtree wires syscall entry, dispatch, xapi bridges, user memory copying, signals, and large user-facing syscall handlers.

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Syscall CPU entry | `syscall.cpp` | Programs EFER/STAR/LSTAR/SYSCALL_MASK. |
| Main syscall table/handlers | `sys.cpp` | Large dispatch surface; high-risk file. |
| File syscalls | `sys.cpp`, `xapi/xfile.cpp` | VFS handles and user copy wrappers. |
| GUI/xapi bridge | `xapi/xgui.cpp` | Graphics/window xapi syscall layer. |
| Signal behavior | `signal.cpp`, `include/syscall/signal.h` | User signal delivery/handling. |
| ABI headers | `include/syscall/`, `user/xapi/include/` | Kernel/user contract. |

## CONVENTIONS
- Syscall entry depends on exact GDT selector offsets; `init_syscall()` validates expected user/kernel selector relationships before enabling syscall.
- User pointers must be copied via current process page-directory aware helpers/bounce buffers, not blindly dereferenced.
- Error returns generally cross from kernel/VFS errors into user-visible xapi/POSIX-like codes; preserve existing conventions per handler family.
- File-related syscalls are tightly coupled to `driver/fs/vfs` and process file descriptor state.
- GUI syscalls bridge to `graphics/` kernel objects; user apps should not reach kernel graphics structs directly.
- Keep `user/xapi/include` declarations synchronized with kernel syscall signatures/semantics.

## ANTI-PATTERNS
- Do not trust user buffers, lengths, or paths at this boundary.
- Do not add syscall numbers or ABI fields in only one side of the kernel/user split.
- Do not refactor large `sys.cpp` handlers while fixing a narrow syscall bug; isolate behavior changes.
- Do not bypass signal/task locking conventions when touching process state.
- Avoid running clangd/clang-tidy blindly on `sys.cpp`; clangd has crashed on this file in this environment.

## VERIFY
Build with `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja all`. For user-visible changes, run the relevant app/test surface (`sigtest.elf`, `posixdemo.elf`, `libctest.elf`, shell/browser) in QEMU.
