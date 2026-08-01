.balign	 0x1000

.text
.code16

_APU_boot_base = .

.global _APU_boot_start
_APU_boot_start:
    jmp _start_ap
    nop
    nop
    nop
    nop
    nop
    nop
page_table_addr:
    .quad 0
entry_addr:
    .quad 0
stack:
    .quad 0
cpu_id:
    .quad 0
tr:
    .quad 0
ready:
    .quad 0

_start_ap:
	cli
	wbinvd

	mov	%cs,	%ax
	mov	%ax,	%ds
	mov	%ax,	%es
	mov	%ax,	%ss
	mov	%ax,	%fs
	mov	%ax,	%gs

	movl	$(_APU_boot_tmp_Stack_end - _APU_boot_base),	%esp

	mov	    %cs,	%ax
	movzx	%ax,	%esi
	shll	$4,	%esi

	leal	(_APU_Code32 - _APU_boot_base)(%esi),	%eax
	movl	%eax,	_APU_Code32_vector - _APU_boot_base

	leal	(_APU_Code64 - _APU_boot_base)(%esi),	%eax
	movl	%eax,	_APU_Code64_vector - _APU_boot_base

	leal	(_APU_tmp_GDT - _APU_boot_base)(%esi),	%eax
	movl	%eax,	(_APU_tmp_GDT + 2 - _APU_boot_base)
	
	lidtl	_APU_tmp_IDT - _APU_boot_base
	lgdtl	_APU_tmp_GDT - _APU_boot_base

	smsw	%ax
	bts	$0	,%ax
	lmsw	%ax

	ljmpl	*(_APU_Code32_vector - _APU_boot_base)

.code32
.balign 4
_APU_Code32:
	mov	$0x10,	%ax
	mov	%ax,	%ds
	mov	%ax,	%es
	mov	%ax,	%ss
	mov	%ax,	%fs
	mov	%ax,	%gs

	leal	(_APU_boot_tmp_Stack_end - _APU_boot_base)(%esi),	%eax
	movl	%eax,	%esp
	
	movl	%cr4,	%eax
	bts	    $5,	%eax
	movl	%eax,	%cr4

	leal	(page_table_addr - _APU_boot_base)(%esi),	%eax
    movl    (%eax), %eax
	movl	%eax,	%cr3

	movl	$0xC0000080,	%ecx
	rdmsr

	bts	    $8,	%eax
	wrmsr

	movl	%cr0,	%eax
	bts	    $0,	%eax
	bts	    $31,	%eax
	movl	%eax,	%cr0
	
	ljmp	*(_APU_Code64_vector - _APU_boot_base)(%esi)


.code64
.balign 4
_APU_Code64:
	movq	$0x20,	%rax
	movq	%rax,	%ds
	movq	%rax,	%es
	movq	%rax,	%fs
	movq	%rax,	%gs
	movq	%rax,	%ss

    leaq    (stack - _APU_boot_base)(%rsi), %rax
    movq    (%rax), %rbp
    movq    (%rax), %rsp

	leaq	(ready - _APU_boot_base)(%rsi), %rax
    movq    $1, (%rax)

    leaq    (cpu_id - _APU_boot_base)(%rsi), %rax
    movq    (%rax), %r14

    leaq    (tr - _APU_boot_base)(%rsi), %rax
    movq    (%rax), %r15

    leaq	(entry_addr - _APU_boot_base)(%rsi), %rax
    movq    (%rax), %r8

	movq 	%r14, %rdi
	movq 	%r15, %rsi

	jmpq	*%r8

.balign 4
_APU_tmp_IDT:
	.word	0
	.word	0,0

.balign 4
_APU_tmp_GDT:
	.short	_APU_tmp_GDT_end - _APU_tmp_GDT - 1
	.long	_APU_tmp_GDT - _APU_boot_base
	.short	0
	.quad	0x00cf9a000000ffff
	.quad	0x00cf92000000ffff
	.quad	0x0020980000000000
	.quad	0x0000920000000000
_APU_tmp_GDT_end:

.balign 4
_APU_Code32_vector:
	.long	_APU_Code32 - _APU_boot_base
	.word	0x08,0	

.balign 4
_APU_Code64_vector:
	.long	_APU_Code64 - _APU_boot_base
	.word	0x18,0	

.balign 4
_APU_boot_tmp_Stack_start:
	.org	0x1000
_APU_boot_tmp_Stack_end:

.global _APU_boot_end
_APU_boot_end:
