    org 0100h

BaseOfStack             equ 0100h
PageDirBase             equ 100000h
PageTblBase             equ 101000h

    jmp LABEL_START
FIN:
		HLT
		JMP FIN
	
%include "fat12hdr.inc"
%include "pm.inc"
%include "load.inc"

LABEL_GDT:          Descriptor 0,            0, 0
LABEL_DESC_FLAT_C:  Descriptor 0,      0fffffh, DA_CR | DA_32 | DA_LIMIT_4K
LABEL_DESC_FLAT_RW: Descriptor 0,      0fffffh, DA_DRW | DA_32 | DA_LIMIT_4K
LABEL_DESC_VIDEO:   Descriptor 0B8000h, 0ffffh, DA_DRW | DA_DPL3

GdtLen equ $ - LABEL_GDT
GdtPtr dw GdtLen - 1
       dd BaseOfLoaderPhyAddr + LABEL_GDT

SelectorFlatC  equ LABEL_DESC_FLAT_C  - LABEL_GDT
SelectorFlatRW equ LABEL_DESC_FLAT_RW - LABEL_GDT
SelectorVideo  equ LABEL_DESC_VIDEO   - LABEL_GDT + SA_RPL3

LABEL_START:
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BaseOfStack

	MOV 	AX,0x1000
	MOV 	ES,AX
	MOV 	CH,0
	MOV 	DH,1
	MOV 	CL,2
	MOV 	AH,02H
	MOV 	AL,1
	MOV 	BX,0
	MOV 	DL,0x00
	INT 	13H
	JC 		FIN
	
	
    call SwitchGraphicMode

    mov word [wSectorNo], SectorNoOfRootDirectory
    xor ah, ah
    xor dl, dl
    int 13h
	
	

	
	
	; mov ah,00h
	; mov al,12h
	; int 10h
	
	
; loader:
		; MOV 	AL,2
		; MOV 	AH,0CH
		; NOP
		; INT 	10H
		; NOP
		; ADD 	DX,1

		; NOP
		; MOV 	AH,0CH
		; NOP
		; INT 	10H
		; NOP
		; ADD 	DX,1

		; SUB 	DX,1
		; NOP
		; ADD		CX,1
		; NOP
		
		; NOP
		; MOV 	ESI,0
		; CALL 	noploop
		; CMP 	CX,270
		; JBE 	loader
		; ;MOV 	AL,0x03
		; ;MOV 	AH,0x00
		; ;INT 	0x10
		; ;mov ax, 0600h
		; ;mov bx, 0700h
		; ;mov cx, 0
		; ;mov dx, 0184fh
		; ;int 10h
		; JMP 	LABEL_SEARCH_IN_ROOT_DIR_BEGIN
; noploop:
		; NOP
		; ADD 	ESI,1
		; CMP 	ESI,10000000
		; JBE 	noploop
		; RET
		
LABEL_SEARCH_IN_ROOT_DIR_BEGIN:
    cmp word [wRootDirSizeForLoop], 0
    jz LABEL_NO_KERNELBIN
    dec word [wRootDirSizeForLoop]
    mov ax, BaseOfKernelFile
    mov es, ax
    mov bx, OffsetOfKernelFile
    mov ax, [wSectorNo]
    mov cl, 1
    call ReadSector

    mov si, KernelFileName
    mov di, OffsetOfKernelFile
    cld
    mov dx, 10h
LABEL_SEARCH_FOR_KERNELBIN:
    cmp dx, 0
    jz LABEL_GOTO_NEXT_SECTOR_IN_ROOT_DIR
    dec dx
    mov cx, 11
LABEL_CMP_FILENAME:
    cmp cx, 0
    jz LABEL_FILENAME_FOUND
    dec cx
    lodsb
    cmp al, byte [es:di]
    jz LABEL_GO_ON
    jmp LABEL_DIFFERENT

LABEL_GO_ON:
    inc di
    jmp LABEL_CMP_FILENAME

LABEL_DIFFERENT:
    and di, 0FFE0h
    add di, 20h
    mov si, KernelFileName
    jmp LABEL_SEARCH_FOR_KERNELBIN

LABEL_GOTO_NEXT_SECTOR_IN_ROOT_DIR:
    add word [wSectorNo], 1
    jmp LABEL_SEARCH_IN_ROOT_DIR_BEGIN

LABEL_NO_KERNELBIN:
    jmp $

LABEL_FILENAME_FOUND:
    mov ax, RootDirSectors
    and di, 0FFF0h

    push eax
    mov eax, [es:di + 01Ch]
    mov dword [dwKernelSize], eax
    pop eax

    add di, 01Ah
    mov cx, word [es:di]
    push cx
    add cx, ax
    add cx, DeltaSectorNo
    mov ax, BaseOfKernelFile
    mov es, ax
    mov bx, OffsetOfKernelFile
    mov ax, cx

LABEL_GOON_LOADING_FILE:
    mov cl, 1
    call ReadSector
    pop ax
    call GetFATEntry
    cmp ax, 0FFFh
    jz LABEL_FILE_LOADED
    push ax
    mov dx, RootDirSectors
    add ax, dx
    add ax, DeltaSectorNo
    add bx, [BPB_BytsPerSec]
    jmp LABEL_GOON_LOADING_FILE
LABEL_FILE_LOADED:
    call KillMotor
    
    lgdt [GdtPtr]

    cli

    in al, 92h
    or al, 00000010b
    out 92h, al

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    
    jmp dword SelectorFlatC:(BaseOfLoaderPhyAddr + LABEL_PM_START)

wRootDirSizeForLoop dw RootDirSectors
wSectorNo           dw 0
bOdd                db 0
TempVramAddr        dd 0

dwKernelSize        dd 0

KernelFileName      db "KERNEL  BIN", 0

ErrNo32BitModeSupported db "Error: this computer doesn't support 1024x768x32bit graphic mode."
LenErrNo32BitModeSupported equ $ - ErrNo32BitModeSupported

ReadSector:
    push bp
    mov bp, sp
    sub esp, 2

    mov byte [bp - 2], cl
    push bx
    mov bl, [BPB_SecPerTrk]
    div bl
    inc ah
    mov cl, ah
    mov dh, al
    shr al, 1
    mov ch, al
    and dh, 1
    pop bx

    mov dl, [BS_DrvNum]
.GoOnReading:
    mov ah, 2
    mov al, byte [bp - 2]
    int 13h
    jc .GoOnReading

    add esp, 2
    pop bp

    ret

GetFATEntry:
    push es
    push bx
    push ax
    mov ax, BaseOfKernelFile
    sub ax, 0100h
    mov es, ax
    pop ax
    mov byte [bOdd], 0
    mov bx, 3
    mul bx
    mov bx, 2
    div bx
    cmp dx, 0
    jz LABEL_EVEN
    mov byte [bOdd], 1
LABEL_EVEN:
    xor dx, dx
    mov bx, [BPB_BytsPerSec]
    div bx
    push dx
    mov bx, 0
    add ax, SectorNoOfFAT1
    mov cl, 2
    call ReadSector

    pop dx
    add bx, dx
    mov ax, [es:bx]
    cmp byte [bOdd], 1
    jnz LABEL_EVEN_2
    shr ax, 4
LABEL_EVEN_2:
    and ax, 0FFFh

LABEL_GET_FAT_ENRY_OK:
    pop bx
    pop es
    ret

KillMotor:
    push dx
    mov dx, 03F2h
    mov al, 0
    out dx, al
    pop dx
    ret

SwitchGraphicMode:
    mov ax, 07e0h
    mov es, ax
    mov ax, 04f00h
    mov di, 0
    int 10h
    mov ax, 0
    mov dword eax, [es:14]
    and eax, 0xffff0000
    shr eax, 16
    mov es, ax
    mov dword eax, [es:14]
    and eax, 0x0000ffff
    mov di, ax
.lp:
    mov word ax, [es:di]
    cmp ax, 0ffffh
    je .end
    add di, 2
    call .check_and_switch
    jz .end
    jmp .lp
.end:
    ret
.get_info:
    pushad
    mov ax, 0700h
    mov es, ax

    mov ax, 04f01h
    add cx, 04000h

    mov di, 0
    int 10h
    popad
    ret
.check_and_switch:
    pushad
    push es
    mov cx, ax
    call .get_info
    
    mov ax, 0700h
    mov es, ax
    mov word ax, [es:18]
    cmp ax, 1024
    jne .end1

    mov word ax, [es:20]
    cmp ax, 768
    jne .end1

    mov byte al, [es:25]
    cmp al, 32
    jne .end1

    jmp .end2
.end1:
    pop es
    popad
    cmp dword esp, 0xffffffff
    ret
.end2:
    call FillInBootInfo

    mov ax, 04f02h
    add cx, 04000h
    mov bx, cx
    int 10h
    
    pop es
    popad

    cmp ax, ax
    ret

FillInBootInfo:
    push ds
    mov ax, 0
    mov ds, ax
    mov word [BootInfoMagic], 0xfaaf
    mov word [BootInfoVmode], 32
    mov word [BootInfoScrnx], 1024
    mov word [BootInfoScrny], 768
    mov eax, dword [es:0x28]
    mov dword [BootInfoVram], eax
    pop ds
    ret

[section .s32]

align 32

[bits 32]

LABEL_PM_START:
    mov ax, SelectorVideo
    mov gs, ax

    mov ax, SelectorFlatRW
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax
    mov esp, TopOfStack

    call InitKernel

    jmp SelectorFlatC:KernelEntryPointPhyAddr

MemCpy:
    push ebp
    mov ebp, esp

    push esi
    push edi
    push ecx

    mov edi, [ebp + 8]
    mov esi, [ebp + 12]
    mov ecx, [ebp + 16]
.1:
    cmp ecx, 0
    jz .2

    mov al, [ds:esi]
    inc esi
    mov byte [es:edi], al
    inc edi

    dec ecx
    jmp .1
.2:
    mov eax, [ebp + 8]

    pop ecx
    pop edi
    pop esi
    mov esp, ebp
    pop ebp

    ret

InitKernel:
    xor esi, esi
    mov cx, word [BaseOfKernelFilePhyAddr + 2Ch]
    movzx ecx, cx
    mov esi, [BaseOfKernelFilePhyAddr + 1Ch]
    add esi, BaseOfKernelFilePhyAddr
.Begin:
    mov eax, [esi + 0]
    cmp eax, 0
    jz .NoAction
    push dword [esi + 010h]
    mov eax, [esi + 04h]
    add eax, BaseOfKernelFilePhyAddr
    push eax
    push dword [esi + 08h]
    call MemCpy
    add esp, 12
.NoAction:
    add esi, 020h
    dec ecx
    jnz .Begin

    ret

[section .data1]
LABEL_DATA:
StackSpace: times 1024 db 0
TopOfStack equ BaseOfLoaderPhyAddr + $

