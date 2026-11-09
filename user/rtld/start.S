/* SPDX-License-Identifier: Apache-2.0 */
.text
.global _start
.hidden _start
.type _start, @function
_start:
    endbr64
    mov %rsp, %rdi
    xor %ebp, %ebp
    and $-16, %rsp
    call rtld_start
    ud2
.size _start, . - _start

.global rtld_enter
.hidden rtld_enter
.type rtld_enter, @function
rtld_enter:
    mov %rdi, %rax
    mov %rsi, %rsp
    xor %ebp, %ebp
    jmp *%rax
.size rtld_enter, . - rtld_enter
.section .note.GNU-stack,"",@progbits
