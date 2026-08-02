# XJ380 Distribution Licenses

This file defines the license material that must accompany an XJ380 source or
binary distribution. The command-line image installs the repository notices at
`/usr/share/doc/xj380/licenses`.

## Source and CLI image components

| Component | License | Source location | Image location |
| --- | --- | --- | --- |
| FatFs | ChaN permissive license | `driver/fs/fatfs/ff.cpp` | `fatfs.txt` |
| lwIP | BSD-style | `kmod/netserver/lwip/` | `lwip.txt` |
| Musl libc `elf.h` | MIT license | `include/elf.h` | `glibc-elf-h.txt` |
| liballoc | MIT license | `liballoc-x86_64.a` origin | `liballoc.txt` |
| parson JSON parser | MIT | `lib/xapi_json_impl.inc`; `third_party/parson` | `third-party compliance bundle` |
| XJ380 project notices | Project notice | `THIRD_PARTY_NOTICES.md` | `THIRD_PARTY_NOTICES.md` |
| MikanOS hankaku | Apache-2.0 license | `font/hankaku.bin` `third_party/mikanos-hankaku/hankaku.txt` | `mikanos.txt` |

## Retained Linux compatibility payload

`complete` keeps its historical Linux compatibility and XBPS staging behavior.
The final image must add the exact license text, package version, source URL,
and any required source offer for every binary copied or installed by that
path:

| Component family | Required material | Image location |
| --- | --- | --- |
| musl | MIT license and matching source provenance | `musl/` |
| GCC runtime and glibc loader | GPL/LGPL notices plus matching source offer | `gcc-runtime/`, `glibc-runtime/` |
| BusyBox | GPL-2.0 license and complete corresponding source offer | `busybox/` |
| Fastfetch | Exact upstream license for the packaged release | `fastfetch/` |
| XBPS and Void packages | Per-package licenses, versions, repositories, and source obligations | `xbps/`, `void-packages/` |

## Release rule

Do not ship a `complete` image until the external binary inputs have been
resolved to exact versions and their license/source material has been copied
to the paths above. `THIRD_PARTY_NOTICES.md` alone is not sufficient.
