/* SPDX-License-Identifier: Apache-2.0 */
/* Shared-XAPI entry. The interpreter supplies the initial stack and RDX fini. */
.text
.global _start
.type _start, @function
_start:
    endbr64
    xor %ebp, %ebp
    mov %rdx, %rcx
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    lea 16(%rsp,%rdi,8), %rdx
    lea main(%rip), %r8
    and $-16, %rsp
    call __xapi_start_main@PLT
    ud2
.size _start, . - _start
.section .note.GNU-stack,"",@progbits
