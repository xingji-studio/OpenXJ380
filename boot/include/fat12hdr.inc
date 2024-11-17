    BS_OEMName     db "XJ380OS "
    BPB_BytsPerSec dw 512
    BPB_SecPerClus db 1
    BPB_RsvdSecCnt dw 1
    BPB_NumFATs    db 2
    BPB_RootEntCnt dw 224
    BPB_TotSec16   dw 2880
    BPB_Media      db 0xF0
    BPB_FATSz16    dw 9
    BPB_SecPerTrk  dw 18
    BPB_NumHeads   dw 2
    BPB_HiddSec    dd 0
    BPB_TotSec32   dd 0
    BS_DrvNum      db 0
    BS_Reserved1   db 0
    BS_BootSig     db 29h
    BS_VolID       dd 0
    BS_VolLab      db "XJ380OSDisk"
    BS_FileSysType db "FAT12   "

FATSz                   equ 9
RootDirSectors          equ 14
SectorNoOfRootDirectory equ 19
SectorNoOfFAT1          equ 1
DeltaSectorNo           equ 17