%include "sconst.inc"

[section .text]

global out_byte, in_byte
global disable_irq, enable_irq
global disable_int, enable_int

out_byte:
    mov edx, [esp + 4]
    mov al, [esp + 8]
    out dx, al
    nop
    nop
    ret

in_byte:
    mov edx, [esp + 4]
    xor eax, eax
    in al, dx
    nop
    nop 
    ret

disable_irq:
    mov ecx, [esp + 4]
    pushf
    cli
    mov ah, 1
    rol ah, cl
    cmp cl, 8
    jae disable_8
disable_0:
    in al, INT_M_CTLMASK
    test al, ah
    jnz dis_already
    or al, ah
    out INT_M_CTLMASK, al
    popf
    mov eax, 1
    ret
disable_8:
    in al, INT_S_CTLMASK
    test al, ah
    jnz dis_already
    or al, ah
    out INT_S_CTLMASK, al
    popf
    mov eax, 1
    ret
dis_already:
    popf
    xor eax, eax
    ret

enable_irq:
    mov ecx, [esp + 4]
    pushf
    cli
    mov ah, ~1
    rol ah, cl
    cmp cl, 8
    jae enable_8
enable_0:
    in al, INT_M_CTLMASK
    and al, ah
    out INT_M_CTLMASK, al
    popf
    ret
enable_8:
    in al, INT_S_CTLMASK
    and al, ah
    out INT_S_CTLMASK, al
    popf
    ret

disable_int:
    cli
    ret

enable_int:
    sti
    ret
