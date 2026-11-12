/* SPDX-License-Identifier: Apache-2.0 */
/* Dynamic native entry: read the initial stack, then call the XAPI-style
 * main(argc, argv, envp). RDX carries the interpreter's finalizer. */
.text
.global _start
.type _start, @function
_start:
    endbr64
    xor %ebp, %ebp
    mov %rdx, %r12
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    lea 16(%rsp,%rdi,8), %rdx
    and $-16, %rsp
    call main
    mov %eax, %ebx
    test %r12, %r12
    jz 1f
    call *%r12
1:
    mov %ebx, %edi
    mov $231, %eax
    syscall
    ud2
.size _start, . - _start
.section .note.GNU-stack,"",@progbits
