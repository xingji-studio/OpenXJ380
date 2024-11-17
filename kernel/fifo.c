#include "include.h"
//FIFO缓冲区相关
PUBLIC void fifo_init(FIFO *fifo, int size, u32 *buf)
{
    fifo->size = size;
    fifo->buf = buf;
    fifo->free = size;
    fifo->flags = 0;
    fifo->p = 0;
    fifo->q = 0;
}

PUBLIC int fifo_put(FIFO *fifo, u32 data)
{
    if (fifo->free == 0) {
        fifo->flags |= FIFO_FLAGS_OVERRUN;
        return -1;
    }
    fifo->buf[fifo->p] = data;
    fifo->p++;
    if (fifo->p == fifo->size) fifo->p = 0;
    fifo->free--;
    return 0;
}

PUBLIC int fifo_get(FIFO *fifo)
{
    int data;
    if (fifo->free == fifo->size) return -1;
    data = fifo->buf[fifo->q];
    fifo->q++;
    if (fifo->q == fifo->size) fifo->q = 0;
    fifo->free++;
    return data;
}

PUBLIC int fifo_status(FIFO *fifo)
{
    return fifo->size - fifo->free;
}