#include "include.h"
//显示光标
PUBLIC void init_mouse_cursor(int *mouse, int bc)
{
    //NEW光标！
    static char cursor[19][16] = {
        "*...............",
		"**..............",
		"*O*.............",
		"*OO*............",
		"*OOO*...........",
		"*OOOO*..........",
		"*OOOOO*.........",
		"*OOOOOO*........",
		"*OOOOOOO*.......",
		"*OOOOOOOO*......",
		"*OOOOOOOOO*.....",
		"*OOOOOOOOOO*....",
		"*OOOOOO*****....",
		"*OOO*OO*........",
		"*OO*.*OO*.......",
		"*O*..*OO*.......",
		"**....*OO*......",
		"......*OO*......",
		".......**.......",
    };
    
    int x, y;
    for (y = 0; y < 19; y++) {
        for (x = 0; x < 16; x++) {
            if (cursor[y][x] == '*') {
                mouse[y * 16 + x] = 0x000000;	//如果是*则填充黑色
            }
            if (cursor[y][x] == 'O') {
                mouse[y * 16 + x] = 0xffffff;	//如果是O则填充白色
            }
            if (cursor[y][x] == '.') {
                mouse[y * 16 + x] = bc;		//如果是.则填充透明色
            }
        }
    }
    return;
}