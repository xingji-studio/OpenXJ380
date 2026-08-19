# Third-Party Notices

XJ380 includes third-party components under their upstream licenses. The
machine-readable inventory is `third_party/compliance-manifest.json`; the build
generates a distributable bundle at `out/compliance/third-party`.

## Components

| Component | Selected license | Material |
|---|---|---|
| musl ELF definitions | MIT | `third_party/musl-elf` |
| lwIP | BSD-style notices | compiled files under `kmod/netserver/lwip`; `licenses/lwip-notice-full.txt` |
| FatFs | BSD-style permissive | `driver/fs/fatfs` |
| libvterm | MIT | `third_party/libvterm/LICENSE` |
| Mbed TLS | Apache-2.0 | `third_party/mbedtls-license-selection.json` |
| Lexbor | Apache-2.0 | `third_party/lexbor/LICENSE` and `NOTICE` |
| Rust alloc/compiler_builtins | Apache-2.0 OR MIT | `third_party/rust-runtime/LICENSE-APACHE`; `third_party/rust-runtime/LICENSE-MIT`; `third_party/rust-runtime/SOURCE.md`; `licenses/liballoc.txt`; `liballoc-x86_64.a` |
| talc allocator | MIT | `third_party/talc/LICENSE.md`; `third_party/talc/SOURCE.md`; `liballoc-x86_64.a` |
| MikanOS hankaku font | Apache-2.0 | `third_party/mikanos-hankaku/LICENSE`; `third_party/mikanos-hankaku/SOURCE.md`; `third_party/mikanos-hankaku/hankaku.txt`; `font/hankaku.bin` |
| MikanOS-derived framebuffer header | Apache-2.0 for derived portions | `include/efi/fbc.h`; `third_party/mikanos-hankaku/LICENSE`; `third_party/mikanos-hankaku/SOURCE.md` |
| Linux UAPI ioctl definitions | GPL-2.0 WITH Linux-syscall-note | `third_party/linux-uapi`; `include/ioctl.h` |

## Project-local replacements

`include/elf.h` is the MIT-licensed musl header. `kmod/netserver/netserver.cpp` and
`kmod/netserver/arch/sys_arch.*` are project-local implementations. The
netserver continues to link the separately licensed BSD-3-Clause lwIP source.

`include/efi/fbc.h` contains portions derived from MikanOS and retains the
Apache-2.0 attribution in the source file. The MikanOS source record is kept in
`third_party/mikanos-hankaku/SOURCE.md`.
