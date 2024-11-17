//Microsoft Fuck You

#include "include.h"

#define SHEET_USE 1     //使用

PUBLIC void shtctl_init(SHTCTL *ctl)    //初始化
{
    memset(ctl, 0, sizeof(ctl));    //开辟内存空间
    ctl->vram = binfo->vram;
    ctl->xsize = binfo->scrnx;
    ctl->ysize = binfo->scrny;
    ctl->top = -1;
    for (int i = 0; i < MAX_SHEETS; i++) {	//小于最大图层量
        ctl->sheets0[i].flags = 0;	//标记为未使用
    }
}

PUBLIC SHEET *sheet_alloc(SHTCTL *ctl)      //ALLOC
{
    SHEET *sht;
    int i;
    for (i = 0; i < MAX_SHEETS; i++) {
        if (ctl->sheets0[i].flags == 0) {	//如果未使用
            sht = &ctl->sheets0[i];
            sht->flags = SHEET_USE;			//标记为使用
            sht->height = -1;
            return sht;
        }
    }

    return NULL;
}

PUBLIC void sheet_setbuf(SHEET *sht, u32 *buf, int xsize, int ysize, int col_inv)   //设置图层
{
    sht->buf = buf;
    sht->bxsize = xsize;
    sht->bysize = ysize;
    sht->col_inv = col_inv;
}

PUBLIC void sheet_updown(SHEET *sht, int height)    //改变图层高度
{
    int h, old = sht->height;
    SHTCTL *ctl = &shtctl;

    if (height > ctl->top + 1) height = ctl->top + 1;
    if (height < -1) height = -1;
    /*
    ctl->sheets0[height].flags = 1;
    ctl->sheets0[old].flags = 0;
    */
    sht->height = height;

    if (old > height) {
        //如果是降低图层
        if (height >= 0) {
            for (h = old; h > height; h--) {
                ctl->sheets[h] = ctl->sheets[h - 1];
                ctl->sheets[h]->height = h;
            }
            ctl->sheets[height] = sht;
        } else {
            if (ctl->top > old) {
                for (h = old; h < ctl->top; h++) {
                    ctl->sheets[h] = ctl->sheets[h + 1];
                    ctl->sheets[h]->height = h;
                }
            }
            ctl->top--;
        }
        sheet_refreshsub(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize);
    } else if (old < height) {
        //如果是增高图层
        if (old >= 0) {
            for (h = old; h < height; h++) {
                ctl->sheets[h] = ctl->sheets[h + 1];
                ctl->sheets[h]->height = h;
            }
            ctl->sheets[height] = sht;
        } else {
            for (h = ctl->top; h >= height; h--) {
                ctl->sheets[h + 1] = ctl->sheets[h];
                ctl->sheets[h + 1]->height = h + 1;
            }
            ctl->sheets[height] = sht;
            ctl->top++;
        }
        sheet_refreshsub(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize);
    }
}

PUBLIC void sheet_refreshsub(SHTCTL *ctl, int vx0, int vy0, int vx1, int vy1)   //刷新图层
{
    int h, bx, by, vx, vy, bx0, by0, bx1, by1;
    u32 *buf, c;
    SHEET *sht;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > ctl->xsize) vx1 = ctl->xsize;
    if (vy1 > ctl->ysize) vy1 = ctl->ysize;
    // 增大刷新范围
    vx0--;
    vy0--;
    vx1++;
    vy1++;
    //ctl->vram[vy * ctl->xsize + vx] = c;
    //
    //vx = sht->vx0 + bx;
    //c = buf[by * sht->bxsize + bx];
    for (h = 0; h <= ctl->top; h++) 
    {
        sht = ctl->sheets[h];
        buf = sht->buf;
        
        bx0 = vx0 - sht->vx0;
        by0 = vy0 - sht->vy0;
        bx1 = vx1 - sht->vx0;
        by1 = vy1 - sht->vy0;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > sht->bxsize) bx1 = sht->bxsize;
        if (by1 > sht->bysize) by1 = sht->bysize;
        for (by = by0; by < by1; by++) {
            vy = sht->vy0 + by;
            for (bx = bx0; bx < bx1; bx++) {
                //重新绘制图层
                vx = sht->vx0 + bx;
                c = buf[by * sht->bxsize + bx];
                if (c ^ sht->col_inv) {
                    ctl->vram[vy * ctl->xsize + vx] = c;
                }
            }
        }
    }
}

PUBLIC void sheet_refresh(SHEET *sht, int bx0, int by0, int bx1, int by1)   //刷新图层
{
    SHTCTL *ctl = &shtctl;
    if (sht->height >= 0) 
    {
    	sheet_refreshsub(ctl, sht->vx0 + bx0, sht->vy0 + by0, sht->vx0 + bx1, sht->vy0 + by1);    //用刷新图层的函数来达到刷新图层的效果
    }
}

PUBLIC void sheet_slide(SHEET *sht, int vx0, int vy0)   //移动图层
{
    SHTCTL *ctl = &shtctl;
    int old_vx0 = sht->vx0, old_vy0 = sht->vy0;
    sht->vx0 = vx0;
    sht->vy0 = vy0;
    if (sht->height >= 0) {	//重新绘制
        sheet_refreshsub(ctl, old_vx0, old_vy0, old_vx0 + sht->bxsize, old_vy0 + sht->bysize);
        sheet_refreshsub(ctl, vx0, vy0, vx0 + sht->bxsize, vy0 + sht->bysize);
    }
}

PUBLIC void sheet_free(SHEET *sht)      //让图层免费（bushi     //关掉，关掉，一定要关掉！再不关掉这些图层，电脑怎有霉譹（hao3）的未来？
{
    if (sht->height >= 0) sheet_updown(sht, -1);
    sht->flags = 0;
}