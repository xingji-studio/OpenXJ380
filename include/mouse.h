#ifndef _XJ380_MOUSE_H_
#define _XJ380_MOUSE_H_

typedef struct {
    u8 buf[3], phase;
    int x, y, btn;
} MOUSE_DEC;

#endif