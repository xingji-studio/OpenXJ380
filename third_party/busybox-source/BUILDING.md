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

The source archive SHA-256 is
`d0f940a72f648943c1f2211e0e3117387c31d765137d92bd8284a3fb9752a998`.
Any local source patch required by a future compiler must be stored next to the
archive and listed here before replacing the distributed binary.
