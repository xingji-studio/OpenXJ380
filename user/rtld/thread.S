/* SPDX-License-Identifier: Apache-2.0 */
.text
.global xj_clone_start
.hidden xj_clone_start
.type xj_clone_start, @function
xj_clone_start:
    endbr64
    and $-16, %rsi
    sub $16, %rsi
    mov %r9, (%rsi)
    mov %rcx, %r10
    mov $56, %eax
    syscall
    test %rax, %rax
    jz 1f
    ret
1:
    xor %ebp, %ebp
    mov (%rsp), %rdi
    call xj_thread_entry
    ud2
.size xj_clone_start, . - xj_clone_start
.section .note.GNU-stack,"",@progbits
