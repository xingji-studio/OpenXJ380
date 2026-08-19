# DRIVER KNOWLEDGE BASE

## OVERVIEW
Built-in driver subtree. Objects under `driver/` are linked into `kernel.krl`, unlike loadable modules in `kmod/`.

## STRUCTURE
```
driver/
├── ahci/, ide/, nvme/    # Storage controllers
├── fs/                   # FATFS, VFS, pseudo-filesystems
├── hda/                  # Audio paths
├── pci/                  # PCI enumeration/config
├── ps2/                  # Keyboard and mouse
└── serial/               # Serial logging/device support
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Device manager | `device.cpp`, `include/device.h` | Built-in device registration. |
| PCI | `pci/`, `include/pci/` | Probing used by storage/audio/module drivers. |
| Storage | `ahci/`, `ide/`, `nvme/` | Initialized from `KernelMain`. |
| Input | `ps2/`, `include/ps2/` | Keyboard/mouse IRQ paths. |
| Audio | `hda/` | HDA sound support. |
| Filesystems | `fs/` | See child `driver/fs/AGENTS.md`. |

## CONVENTIONS
- Use `driver/` only for drivers that must be available in the core kernel image.
- If a device driver is intended to be loadable and optional, put it in `kmod/` and follow `.sys`/`dlmain` rules.
- Driver initialization order is mostly controlled by `kernel/main.cpp`; storage and VFS setup are boot-critical.
- Shared public driver ABI lives in `include/`; keep headers minimal and freestanding.
- Serial logging is the primary early-debug surface; preserve useful boot messages around risky init stages.

## ANTI-PATTERNS
- Do not add a built-in driver solely because it is easier to link; respect module vs built-in placement.
- Do not assume scheduler, rootfs, or user processes exist during early storage/input initialization.
- Do not silently change device registration names or node paths; user apps and VFS may depend on them.

## VERIFY
`python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja all`; hardware-visible changes need QEMU boot and `serial.log`. Storage changes should also build `ninja -f build.ninja vdisk`.
