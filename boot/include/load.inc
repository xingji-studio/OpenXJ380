BaseOfLoader            equ 09000h
OffsetOfLoader          equ 0100h

BaseOfLoaderPhyAddr     equ BaseOfLoader * 10h

BaseOfKernelFile        equ 08000h
OffsetOfKernelFile      equ 0h

BaseOfKernelFilePhyAddr equ BaseOfKernelFile * 10h
KernelEntryPointPhyAddr equ 0x30400

BootInfoMagic           equ 0xb02
BootInfoVmode           equ 0xb04
BootInfoScrnx           equ 0xb06
BootInfoScrny           equ 0xb08
BootInfoVram            equ 0xb0a

VBEMode equ 0x180