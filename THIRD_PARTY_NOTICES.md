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
| dr_mp3 | MIT-0 OR Unlicense | `include/dr_mp3.h` |
| libvterm | MIT | `third_party/libvterm/LICENSE` |
| Mbed TLS | Apache-2.0 | `third_party/mbedtls-license-selection.json` |
| Lexbor | Apache-2.0 | `third_party/lexbor/LICENSE` and `NOTICE` |
| Rust alloc/compiler_builtins | Apache-2.0 OR MIT | `third_party/rust-runtime/LICENSE-APACHE`; `third_party/rust-runtime/LICENSE-MIT`; `third_party/rust-runtime/SOURCE.md`; `licenses/liballoc.txt`; `liballoc-x86_64.a` |
| talc allocator | MIT | `third_party/talc/LICENSE.md`; `third_party/talc/SOURCE.md`; `liballoc-x86_64.a` |
| BusyBox 1.31.1 | GPL-2.0-only | `third_party/busybox-source` |
| MikanOS hankaku font | Apache-2.0 | `third_party/mikanos-hankaku/LICENSE`; `third_party/mikanos-hankaku/SOURCE.md`; `third_party/mikanos-hankaku/hankaku.txt`; `font/hankaku.bin` |
| MikanOS-derived framebuffer header | Apache-2.0 for derived portions | `include/efi/fbc.h`; `third_party/mikanos-hankaku/LICENSE`; `third_party/mikanos-hankaku/SOURCE.md` |
| libutf | MIT | `third_party/libutf/LICENSE`; `third_party/libutf/SOURCE.md`; `kernel/utflib.cpp`; `include/proto.hpp` |
| Linux UAPI ioctl definitions | GPL-2.0 WITH Linux-syscall-note | `third_party/linux-uapi`; `include/ioctl.h` |

The BusyBox bundle includes the complete upstream source archive, GPLv2 text,
build configuration, compiler-compatibility patch, and rebuild instructions.
The two shipped BusyBox paths are built from that recorded material and must
remain byte-identical.

## Project-local replacements

`include/elf.h` is the MIT-licensed musl header. `include/dma.h`,
`driver/dma.cpp`, `driver/dma_plan.h`, `kmod/netserver/netserver.cpp`, and
`kmod/netserver/arch/sys_arch.*` are project-local implementations. The
netserver continues to link the separately licensed BSD-3-Clause lwIP source.

`include/efi/fbc.h` contains portions derived from MikanOS and retains the
Apache-2.0 attribution in the source file. The MikanOS source record is kept in
`third_party/mikanos-hankaku/SOURCE.md`.

## Native ext4 BSD design references

The native ext4 cache/block-I/O/journal design review consults three lwext4
files bearing BSD-3-Clause notices. No lwext4 implementation code or library is
copied, translated or linked. The observed upstream copyrights, conditions and
disclaimer are retained in `driver/fs/ext4/LWEXT4_BSD_NOTICE.txt` in OpenXJ380;
`driver/fs/ext4/REFERENCES.json` records the consulted views and retrieval limits.
Both are distributed under `/system/licenses/native-ext4`. The native source
remains Apache-2.0; upstream GPL extent/xattr implementations remain excluded.
