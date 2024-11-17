#ifndef _XJ380_FIFO_H_
#define _XJ380_FIFO_H_

typedef struct s_fifo {
    u32 *buf;
    int p, q, size, free, flags;
} FIFO;

#define FIFO_FLAGS_OVERRUN 1

#endif