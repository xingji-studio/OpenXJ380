# BusyBox 1.31.1 corresponding source

The prebuilt BusyBox distributed in `resources/apps/busybox` and
`third_party/busybox-prebuilt/busybox_amd64` reports version 1.31.1. Its complete
upstream source archive, GPL-2.0-only license, and the configuration used for the
reproducible replacement build are kept in this directory.

## Rebuild

Run `./build.sh [output-path]`. The script applies the recorded compiler
compatibility patch, installs the fixed configuration, uses a stable build
identity, disables the volatile Kconfig timestamp, and produces the static
x86_64 binary.

## Disabled applets

This build disables `wget`, `lzma`, `unlzma`, `unxz`, `xz`, `xzcat`, and
transparent LZMA/XZ handling. BusyBox 1.31.1 is retained for image
compatibility, but those applets are removed from the distributed binary to
avoid shipping the vulnerable request-smuggling and decompression paths tracked
by issue #21.

The hardened binary SHA-256 is
`a08214e46cafb238685f694a2ff4e4b038b5fed83f884354127ba08d68498066`.

The source archive SHA-256 is
`d0f940a72f648943c1f2211e0e3117387c31d765137d92bd8284a3fb9752a998`.
Any local source patch required by a future compiler must be stored next to the
archive and listed here before replacing the distributed binary.
