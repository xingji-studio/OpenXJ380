/* SPDX-License-Identifier: Apache-2.0 */
/* Native ELF ABI v1. The interpreter path selects the kernel entry ABI;
 * this note lets the user-space linker reject incompatible native objects. */
.section .note.xj380.abi,"a",@note
.balign 4
.long 6
.long 4
.long 1
.asciz "XJ380"
.balign 4
.long 1
.section .note.GNU-stack,"",@progbits
