.global _start
.type _start, @function
_start:
    endbr64
    jmp xapi_start

.size _start, . - _start

# Native register entry ABI; retained when the executable is stripped.
.section .note.xj380.environment,"",@note
.balign 4
.long 6, 4, 0x58454e56
.asciz "XJ380"
.balign 4
.long 1

.section .note.GNU-stack,"",@progbits
