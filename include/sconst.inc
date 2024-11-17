P_STACKBASE  equ 0
GSREG        equ P_STACKBASE
FSREG        equ GSREG        + 4
ESREG        equ FSREG        + 4
DSREG        equ ESREG        + 4
EDIREG       equ DSREG        + 4
ESIREG       equ EDIREG       + 4
EBPREG       equ ESIREG       + 4
KERNELESPREG equ EBPREG       + 4
EBXREG       equ KERNELESPREG + 4
EDXREG       equ EBXREG       + 4
ECXREG       equ EDXREG       + 4
EAXREG       equ ECXREG       + 4
RETADR       equ EAXREG       + 4
EIPREG       equ RETADR       + 4
CSREG        equ EIPREG       + 4
EFLAGSREG    equ CSREG        + 4
ESPREG       equ EFLAGSREG    + 4
SSREG        equ ESPREG       + 4
P_STACKTOP   equ SSREG        + 4

P_LDT_SEL    equ P_STACKTOP
P_LDT        equ P_LDT_SEL    + 4

TSS3_S_SP0   equ 4

INT_M_CTL     equ 0x20
INT_M_CTLMASK equ 0x21
INT_S_CTL     equ 0xA0
INT_S_CTLMASK equ 0xA1

EOI           equ 0x20

SELECTOR_FLAT_C    equ 0x08
SELECTOR_TSS       equ 0x20

SELECTOR_KERNEL_CS equ SELECTOR_FLAT_C