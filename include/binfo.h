#ifndef _XJ380_BINFO_H_
#define _XJ380_BINFO_H_

struct s_bootinfo {
    u16 magic, vmode, scrnx, scrny;
    u32 *vram;
} __attribute__((packed));

typedef struct s_bootinfo BOOTINFO;

#endif