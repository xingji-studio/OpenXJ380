# Linux UAPI ioctl source record

`include/ioctl.h` contains ioctl constants and framebuffer/terminal ABI structs
that mirror Linux UAPI headers. Linux UAPI headers are distributed under
`GPL-2.0 WITH Linux-syscall-note`; the syscall-note exception permits use of the
exported user/kernel interface definitions without imposing GPL terms on code
that only uses the interface.

Primary upstream references:

- <https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/ioctls.h>
- <https://github.com/torvalds/linux/blob/master/include/uapi/linux/fb.h>
- <https://github.com/torvalds/linux/blob/master/include/uapi/linux/sockios.h>
- <https://github.com/torvalds/linux/blob/master/include/uapi/linux/tty.h>
- <https://github.com/torvalds/linux/blob/master/LICENSES/preferred/GPL-2.0>
- <https://github.com/torvalds/linux/blob/master/LICENSES/exceptions/Linux-syscall-note>

Recorded material hashes:

```text
fb5a425bd3b3cd6071a3a9aff9909a859e7c1158d54d32e07658398cd67eb6a0  third_party/linux-uapi/COPYING
8780e78a1a737e127f25a65f6d95269bffd36158dc261114de7859b490bfc5aa  third_party/linux-uapi/GPL-2.0
8e378ab93586eb55135d3bc119cce787f7324f48394777d00c34fa3d0be3303f  third_party/linux-uapi/Linux-syscall-note
```
