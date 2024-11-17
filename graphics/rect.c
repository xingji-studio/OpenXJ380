#include "include.h"
//画方块
PUBLIC void draw_rect(int x0, int y0, int x1, int y1, int c)
{
    boxfill8(buf_back, binfo->scrnx, c, x0, y0, x1, y1);    //用画方块函数实现画方块的效果
}
//画方块
PUBLIC void boxfill8(u32 *buf, int xsize, int c, int x0, int y0, int x1, int y1)
{
    int x, y;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            buf[y * xsize + x] = c;
        }
    }
}