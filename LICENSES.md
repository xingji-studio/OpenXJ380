# XJ380 Kernel Test Image Licenses

This file defines the license material that must accompany an XJ380 source or
binary distribution. The test image installs the repository notices at
`/system/licenses`.

## Source and CLI image components

| Component | License | Source location | Image location |
| --- | --- | --- | --- |
| XJ380 project | Apache-2.0 | `LICENSE` | `LICENSE` |
| FatFs | ChaN permissive license | `driver/fs/fatfs/ff.cpp` | `fatfs.txt` |
| lwIP | BSD-style | compiled files under `kmod/netserver/lwip/` | `lwip-notice-full.txt` |
| musl ELF definitions | MIT | `include/elf.h`; `third_party/musl-elf` | `musl-elf.txt`; `third-party compliance bundle` |
| Rust alloc/compiler_builtins archive | Apache-2.0 OR MIT | `liballoc-x86_64.a`; `third_party/rust-runtime` | `liballoc.txt`; `third-party compliance bundle` |
| talc allocator | MIT | `liballoc-x86_64.a`; `third_party/talc` | `third-party compliance bundle` |
| Linux UAPI ioctl definitions | GPL-2.0 WITH Linux-syscall-note | `include/ioctl.h`; `third_party/linux-uapi` | `third-party compliance bundle` |
| libutf | MIT | `kernel/utflib.cpp`; `include/proto.hpp`; `third_party/libutf` | `third-party compliance bundle` |
| XJ380 project notices | Project notice | `THIRD_PARTY_NOTICES.md` | `THIRD_PARTY_NOTICES.md` |
| MikanOS hankaku font | Apache-2.0 | `third_party/mikanos-hankaku`; `font/hankaku.bin` | `third-party compliance bundle` |
| MikanOS-derived framebuffer header | Apache-2.0 for derived portions | `include/efi/fbc.h`; `third_party/mikanos-hankaku/SOURCE.md` | `third-party compliance bundle` |
