/* SPDX-License-Identifier: Apache-2.0 */
/* Native ELF ABI v1/v2 (TLS)/v3 (C++ thread_local destruction).
 * The interpreter path selects the kernel entry ABI;
 * this note lets the user-space linker reject incompatible native objects. */
#ifndef XJ380_NATIVE_ABI
#define XJ380_NATIVE_ABI 1
#endif
.section .note.xj380.abi,"a",@note
.balign 4
.long 6
.long 4
.long 1
.asciz "XJ380"
.balign 4
.long XJ380_NATIVE_ABI
.section .note.GNU-stack,"",@progbits
