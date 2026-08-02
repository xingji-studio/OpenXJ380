# Third-Party Notices

XJ380 includes third-party components under their upstream licenses. The
machine-readable inventory is `third_party/compliance-manifest.json`; the build
generates a distributable bundle at `out/compliance/third-party`.

## Components

| Component | Selected license | Material |
|---|---|---|
| musl ELF definitions | MIT | `third_party/musl-elf` |
| lwIP | BSD-3-Clause | `kmod/netserver/lwip` |
| FatFs | BSD-style permissive | `driver/fs/fatfs` |
| stb libraries | MIT OR Unlicense | embedded in headers |
| dr_mp3 | MIT-0 OR Unlicense | `include/dr_mp3.h` |
| litehtml | BSD-3-Clause | `third_party/litehtml/LICENSE` |
| Gumbo Parser | Apache-2.0 | `third_party/litehtml/src/gumbo/LICENSE` |
| libvterm | MIT | `third_party/libvterm/LICENSE` |
| libwebp | BSD-3-Clause | embedded in source headers |
| NanoSVG | Zlib | embedded in source headers |
| Mbed TLS | Apache-2.0 | `third_party/mbedtls-license-selection.json` |
| Lexbor | Apache-2.0 | `third_party/lexbor/LICENSE` and `NOTICE` |
| StardustUI | MIT | `frameworks/StardustUI/LICENSE` |
| RapidJSON | MIT | embedded in `rapidjson.h` |
| Rust alloc/compiler_builtins | Apache-2.0 OR MIT | `third_party/rust-runtime` |
| BusyBox 1.31.1 | GPL-2.0-only | `third_party/busybox-source` |
| MikanOS hankaku.bin | Apache-2.0 | `font/hankaku.bin` |
| maple-font | SIL OPEN FONT LICENSE | `font/ttf/XJ380C.ttf` |
| Source Han Sans font | SIL OPEN FONT LICENSE | `font/ttf/XJ380F.ttf` |
| libutf | MIT | `kernel/utflib.cpp` |

The BusyBox bundle includes the complete upstream source archive, GPLv2 text,
build configuration, compiler-compatibility patch, and rebuild instructions.
The two shipped BusyBox paths are built from that recorded material and must
remain byte-identical.

## Project-local replacements

`include/elf.h` is the MIT-licensed musl header. `include/dma.h`,
`driver/dma.cpp`, `driver/dma_plan.h`, `kmod/netserver/netserver.cpp`, and
`kmod/netserver/arch/sys_arch.*` are project-local implementations. The
netserver continues to link the separately licensed BSD-3-Clause lwIP source.
