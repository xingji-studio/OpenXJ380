# Third-Party Notices

This repository contains third-party source code and headers that remain under
their respective upstream licenses. Binary distributions should reproduce the
required notices for components that require attribution in documentation or
other shipped materials.

## Included third-party components

- `system/kmod/netserver/lwip`
  - Upstream: lwIP TCP/IP stack
  - License style: BSD-like redistribution notice carried in individual files
  - Distribution note: binary redistribution must reproduce the upstream notice
    and disclaimer in documentation and/or other accompanying materials

- `driver/fs/fatfs`
  - Upstream: FatFs by ChaN
  - License style: permissive source/binary redistribution notice in file
    headers

- `graphics/image/stbi.h`
  - Upstream: `stb_image`
  - License style: MIT or public domain, as embedded in the file

- `graphics/image/stbir.h`
  - Upstream: `stb_image_resize`
  - License style: MIT or public domain, as embedded in the file

- `include/dr_mp3.h`
  - Upstream: `dr_mp3`
  - License style: public domain or MIT-0, as embedded in the file

- `include/elf.h`
  - Upstream: GNU C Library `elf.h`
  - License style: LGPL-2.1-or-later, as embedded in the file
  - Distribution note: ship the corresponding LGPL notice text with products
    that redistribute this header or binaries built from it

- `third_party/litehtml`
  - Upstream: litehtml
  - License style: BSD-3-Clause
  - See: `third_party/litehtml/LICENSE`

- `third_party/mbedtls-src`
  - Upstream: Mbed TLS
  - License style: Apache-2.0 OR GPL-2.0-or-later
  - Project intent: consume Mbed TLS under Apache-2.0
  - See: `third_party/mbedtls-src/LICENSE`

- `liballoc-x86_64.a`
  - Source origin: downloaded by `Makefile` from the `plos-clan/liballoc`
    release URL
  - Compliance note: keep the upstream source and license text available when
    distributing builds that include this archive

- `font/hankaku.bin`, `include/efi/fbc.h`, `boot/include/fbc.h`,
  `include/graphics/GOP.hpp`
  - Upstream: MikanOS by Yuuki Uchida (uchan-nos)
  - License style: Apache-2.0
  - See: `third_party/mikanos/LICENSE`, `third_party/mikanos/NOTICE`
  - Compliance note: derived from MikanOS (`kernel/hankaku.txt`,
    `kernel/frame_buffer_config.hpp`, `kernel/graphics.hpp`); upstream
    attribution notices are retained in the file headers. Redistribution must
    reproduce the Apache-2.0 text and notices.

## Locally rewritten code

The following files were rewritten in-tree to remove ambiguous inherited
provenance and are intended to be treated as project-local implementations:

- `include/dma.h`
- `driver/dma.cpp`
- `system/kmod/netserver/arch/sys_arch.cpp`
- `system/kmod/netserver/arch/utils.cpp`
