#ifndef _XJ380_GLOBAL_H_
#define _XJ380_GLOBAL_H_

#ifdef GLOBAL_VARIABLES_HERE
#undef EXTERN
#define EXTERN
#endif

EXTERN u8         gdt_ptr[6];
EXTERN DESCRIPTOR gdt[GDT_SIZE];

EXTERN u8         idt_ptr[6];
EXTERN GATE       idt[IDT_SIZE];

EXTERN TSS        tss;
EXTERN PROCESS   *p_proc_ready;

EXTERN int        k_reenter;
EXTERN int        ticks;
//键鼠相关
EXTERN FIFO       keyfifo;
EXTERN u32        keybuf[32];
EXTERN FIFO       decoded_key;
EXTERN u32        dkey_buf[32];

EXTERN FIFO       mousefifo;
EXTERN u32        mousebuf[32];

EXTERN SHTCTL     shtctl;

extern BOOTINFO  *binfo;
extern char       font[4096];
extern PROCESS    proc_table[];
extern char       task_stack[];
extern TASK       task_table[];

extern irq_handler irq_table[];

extern u32        keymap[];
extern u32       *buf_back;

#endif