#include "include.h"
//鼠标
int mcursor[304];
MOUSE_DEC mdec;
int mx = (1024 - 16) / 2;
int my = (768 - 28 - 19) / 2;
int mmx = -1, mmy = -1, x, y, j;
int ComputerLogoLight = 0;
int CommandLogoLight = 0;
int StartMenuState = 0;

SHEET *sht_mouse, *sht = NULL;

void StartMenuThings()
{
	if (StartMenuState == 0)
	{
		StartMenuState = 1;
		DrawStartMenu(0xeeeeee);
	}
	else if (StartMenuState == 2)
	{
		StartMenuState = 1;
		DrawStartMenu(0xaaaaaa);
	}
	else if (StartMenuState == 1)
	{
		StartMenuState = 0;
		DrawStartMenu(0x44cef7);
	}
}
//鼠标移动
PUBLIC void mouse_handler()
{
    u8 data = in_byte(KB_DATA);
	if (StartMenuState == 2 && ((mdec.btn & 0x01) == 0))
	{
		StartMenuThings();
		//DrawStar(150,150,3,0xffffff);
		//UpExplorer();
	}
    if (mouse_decode(&mdec, data)) {
        mx += mdec.x;
        my += mdec.y;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > binfo->scrnx - 1) mx = binfo->scrnx - 1;
        if (my > binfo->scrny - 1) my = binfo->scrny - 1;
        sheet_slide(sht_mouse, mx, my);
		//电脑图标
		if (mx > 10 && mx < 80)
		{
			if (my > 10 && my < 70)
			{
				if (ComputerLogoLight == 0)
				{
					ComputerLogoLight = 1;
					draw_rect(0, 0, 90, 90, 0x55dff7);
					computer();
					reDrawComputer();				
				}

			}else{
				if (ComputerLogoLight == 1)
				{
					ComputerLogoLight = 0;
					draw_rect(0, 0, 90, 90, 0x44cef6);
					computer();
					reDrawComputer();				
				}	
			}
		}else{
			if (ComputerLogoLight == 1)
			{
				ComputerLogoLight = 0;
				draw_rect(0, 0, 90, 90, 0x44cef6);
				computer();
				reDrawComputer();				
			}				
		}
		//命令行图标
		if (mx > 10 && mx < 80)
		{
			if (my > 80 && my < 150)
			{
				if (CommandLogoLight == 0)
				{
					CommandLogoLight = 1;
					draw_rect(0, 90, 90, 180, 0x55dff7);
					DrawCommand();
					reDrawCommand();				
				}

			}else{
				if (CommandLogoLight == 1)
				{
					CommandLogoLight = 0;
					draw_rect(0, 90, 90, 180, 0x44cef6);
					DrawCommand();
					reDrawCommand();				
				}	
			}
		}else{
			if (CommandLogoLight == 1)
			{
				CommandLogoLight = 0;
				draw_rect(0, 90, 90, 180, 0x44cef6);
				DrawCommand();
				reDrawCommand();				
			}				
		}
        if ((mdec.btn & 0x01) != 0) {
			if (ComputerLogoLight == 1)
			{
				UpExplorer();
			}
			if (CommandLogoLight == 1)
			{
				UpCommand();
			}
			if (mx > 0 && mx < 90 && my > binfo->scrny - 40)
			{
				StartMenuThings();
				UpCommand();
			}
            if (mmx < 0) {
                for (j = shtctl.top - 1; j > 0; j--) {
                    sht = shtctl.sheets[j];
                    x = mx - sht->vx0;
                    y = my - sht->vy0;
                    if (0 <= x && x < sht->bxsize && 0 <= y && y < sht->bysize) {
                        if (sht->buf[y * sht->bxsize + x] != sht->col_inv) {//窗口标题栏
                            sheet_updown(sht, shtctl.top - 1);
                            if (3 <= x && x < sht->bxsize - 3 && 3 <= y && y < 21) {
                                mmx = mx;
                                mmy = my;
                            }
							//关闭按钮
                            if (sht->bxsize - 21 <= x && x <= sht->bxsize - 5 && 5 <= y && y < 19) {
                                sheet_updown(sht, -1);//将图层移到最下面
                            }
                            break;
                        }
                    }
                }
            } else {
                x = mx - mmx;
                y = my - mmy;
                sheet_slide(sht, sht->vx0 + x, sht->vy0 + y);
                mmx = mx;
                mmy = my;
            }
        } else {
            mmx = -1;

        }
    }
}
//鼠标中断
PUBLIC void init_mouse()
{
    sht_mouse = sheet_alloc(&shtctl);
    init_mouse_cursor(mcursor, 99);
    sheet_setbuf(sht_mouse, mcursor, 16, 19, 99);
    sheet_slide(sht_mouse, mx, my);
    sheet_updown(sht_mouse, 3);

    kb_wait();
    out_byte(KB_CMD, 0x60);
    kb_wait();
    out_byte(KB_DATA, 0x47);

    kb_wait();
    out_byte(KB_CMD, 0xd4);
    kb_wait();
    out_byte(KB_DATA, 0xf4);

    mdec.phase = 0;
    
    put_irq_handler(MOUSE_IRQ, mouse_handler);
    enable_irq(CASCADE_IRQ);
    enable_irq(MOUSE_IRQ);
}
//鼠标按键识别
PUBLIC int mouse_decode(MOUSE_DEC *mdec, u8 data)
{
    if (mdec->phase == 0) {
        if (data == 0xfa) {
            mdec->phase = 1;
        }
        return 0;
    }
    if (mdec->phase == 1) {
        if ((data & 0xc8) == 0x08) {
            mdec->buf[0] = data;
            mdec->phase = 2;
        }
        return 0;
    }
    if (mdec->phase == 2) {
        mdec->buf[1] = data;
        mdec->phase = 3;
        return 0;
    } 
    if (mdec->phase == 3) {
        mdec->buf[2] = data;
        mdec->phase = 1;
        mdec->btn = mdec->buf[0] & 0x07;
        mdec->x = mdec->buf[1];
        mdec->y = mdec->buf[2];

        if ((mdec->buf[0] & 0x10) != 0) mdec->x |= 0xffffff00;
        if ((mdec->buf[0] & 0x20) != 0) mdec->y |= 0xffffff00;
        mdec->y = -mdec->y;
        return 1;
    }
    return -1;
}

PUBLIC void mouse_sheet_update_to(u16 height){
    sheet_updown(sht_mouse, height);
}