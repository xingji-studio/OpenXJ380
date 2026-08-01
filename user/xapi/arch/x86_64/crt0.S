.global _start
.type _start, @function
_start:
    endbr64
    jmp xapi_start

.size _start, . - _start

.section .note.GNU-stack,"",@progbits
