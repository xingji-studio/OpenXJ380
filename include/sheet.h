#ifndef _XJ380_SHEET_H_
#define _XJ380_SHEET_H_

typedef struct {
    u32 *buf;
    int bxsize, bysize, vx0, vy0, col_inv, height, flags;
} SHEET;

#define MAX_SHEETS 256

typedef struct {
    u32 *vram;
    int xsize, ysize, top;
    SHEET *sheets[MAX_SHEETS];
    SHEET sheets0[MAX_SHEETS];
} SHTCTL;

#endif