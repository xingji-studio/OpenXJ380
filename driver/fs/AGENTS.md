# DRIVER FS KNOWLEDGE BASE

## OVERVIEW
Filesystem subtree: FATFS port, VFS core, device/proc/tmp/pipe/pty/socket-like nodes, and path/node lifecycle code.

## STRUCTURE
```
driver/fs/
├── fatfs/      # FatFs port and Unicode glue
└── vfs/        # VFS core plus pseudo-filesystems
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| VFS core | `vfs/vfs.cpp`, `include/fs/vfs/vfs.h` | Node operations, aliases, dispatch. |
| FATFS port | `fatfs/ff.cpp`, `fatfs/ffunicode.cpp` | Large upstream-derived filesystem code. |
| devfs/procfs | `vfs/dev.cpp`, `vfs/procfs.cpp` | Active kernel pseudo-filesystems. |
| Pipes/PTY/tmp | `vfs/pipefs.cpp`, `vfs/pty.cpp`, `vfs/tmpfs.cpp` | Runtime pseudo-filesystems. |
| Partition layer | `partition.cpp`, `include/fs/partition.h` | Root mount prerequisites. |

## CONVENTIONS
- `KernelMain` initializes `vfs_init()`, `fatfs_init()`, `devfs_setup()`, storage drivers, then `partition_init()` before `mount_root()`.
- VFS aliases are project-specific and protected by locks; preserve alias resolution semantics.
- VFS nodes carry callback tables and fsid-specific behavior. Check existing node lifecycle before changing close/read/write/update paths.
- FATFS files are large and partly upstream-derived; prefer adapter/glue changes over broad rewrites.
- User-facing filesystem behavior crosses into `kernel/syscall/` and `user/xapi/`; update both sides for ABI changes.

## ANTI-PATTERNS
- Do not dereference user pointers in filesystem syscalls; copy at the syscall boundary first.
- Do not change path tokenization or alias behavior without checking the shell and image layout.
- Do not treat FATFS code as ordinary first-party style cleanup territory.
- Do not add pseudo-filesystem globals without considering scheduler/locking and process lifetime.

## VERIFY
Build with `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja all`. Filesystem/rootfs changes need `ninja -f build.ninja vdisk` and QEMU boot; inspect `/apps`, `/system`, `/mod`, and `serial.log` behavior.
