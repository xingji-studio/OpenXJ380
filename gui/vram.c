#include "include.h"

//星星图标(星际工作室)
static char Star[24][24] =
{
	"            +           ",
	"           + +          ",
	"           + +          ",
	"           + +          ",
	"          +   +         ",
	"          +   +         ",
	"          +   +         ",
	"++++++++++++++++++++++++",
	" ++      +     +      + ",
	"   +     +     +    ++  ",
	"    +   +       +  +    ",
	"     ++ +       + +     ",
	"       +         +      ",
	"       ++      +++      ",
	"       + ++   +  +      ",
	"      +    + +    +     ",
	"      +    ++     +     ",
	"      +   +  ++   +     ",
	"     +   +     +   +    ",
	"     +  +       +  +    ",
	"     +++         +++    ",
	"    ++             ++   ",
	"    +               +   "
};

//XJ380
static char XJ380_LOGO[24][24] =
{
	"                        ",
	"                        ",
	"                      ##",
	"                  ######",
	"              ##########",
	"          ##############",
	"      ##################",
	"  ##############@@######",
	"###########@######@@####",
	"###########@########@###",
	"###########@@#@@####@###",
	"##########@@@@@#####@@##",
	"####@##@@@@##@#####@@@##",
	"###@@@##@@@#@@#####@@@##",
	"###@@@####@@@#@#####@###",
	"###@@####@#@@@@#########",
	"####@####@@###@#########",
	"####@####@##############",
	"#####@@#################",
	"#######@@###############",
	"########################",
	"                        ",
	"                        ",
	"                        "
};

PUBLIC void DrawStar(int x, int y, int size, int color)
{
	for (int i = 0; i <= 23; i++)
	{
		for (int j = 0; j <= 24; j++)
		{
			if (Star[j][i] == '+')
			{
				draw_rect(x + i * size, y + j * size, x + i * size + size, y + j * size + size, color);
			}
		}
	}
}
PUBLIC void DrawLOGO(int x, int y, int size, int back_color)
{
	for (int i = 0; i <= 23; i++)
	{
		for (int j = 0; j <= 24; j++)
		{
			if (XJ380_LOGO[j][i] == ' ')
			{
				draw_rect(x + i * size, y + j * size, x + i * size + size, y + j * size + size, back_color);
			}
			else if (XJ380_LOGO[j][i] == '#')
			{
				draw_rect(x + i * size, y + j * size, x + i * size + size, y + j * size + size, 0x00a2e8);
			}
			else if (XJ380_LOGO[j][i] == '@')
			{
				draw_rect(x + i * size, y + j * size, x + i * size + size, y + j * size + size, 0xfff200);
			}
		}
	}
}

PRIVATE void warning()
{
    int x = binfo->scrnx;
    int y = binfo->scrny;

	putstr_atbuf(buf_back, x, x - 449, y - 87, 0x000000, "                XINGJI Studios Caution                 ");
	putstr_atbuf(buf_back, x, x - 449, y - 71, 0x000000, "This version is in beta,Leakage is strictly prohibited.");
	putstr_atbuf(buf_back, x, x - 449, y - 55, 0x000000, "Violators are punished by punishment or even dismissal.");

	putstr_atbuf(buf_back, x, x - 448, y - 88, 0xffffff, "                XINGJI Studios Caution                 ");
	putstr_atbuf(buf_back, x, x - 448, y - 72, 0xffffff, "This version is in beta,Leakage is strictly prohibited.");
	putstr_atbuf(buf_back, x, x - 448, y - 56, 0xffffff, "Violators are punished by punishment or even dismissal.");
}

PUBLIC void init_screen()   //绘制桌面
{
    draw_rect(0, 0, binfo->scrnx - 1, binfo->scrny - 1, 0x44cef6);
	computer();
	DrawCommand();
    draw_rect(0, binfo->scrny - 40, binfo->scrnx - 1, binfo->scrny - 1,  0xc6c6c6);
	draw_rect(0, binfo->scrny - 40, 90, binfo->scrny - 1,  0xaaaaaa);
	DrawStar(5, binfo->scrny - 32, 1, 0x44cef7);
	putstr_atbuf(buf_back, binfo->scrnx, 35, binfo->scrny - 25, 0xffffff, "Menu");
	warning();
}

PUBLIC void DrawStartMenu(int color)
{
	draw_rect(0, binfo->scrny - 360, 270, binfo->scrny - 40, color);
	extern SHEET *sht_backe;
	sheet_refresh(sht_backe, 0, binfo->scrny - 360, 270, binfo->scrny-40);
}

PUBLIC void computer()
{
	draw_rect(10, 10, 80, 55, 0xaaaaaa);//电脑图标
	draw_rect(15, 15, 75, 50, 0x00ffff);
	draw_rect(40, 55, 50, 65, 0xaaaaaa);
	draw_rect(20, 65, 70, 70, 0xaaaaaa);
	putstr_atbuf(buf_back, binfo->scrnx, 14, 74, 0x000000, "Computer");
	putstr_atbuf(buf_back, binfo->scrnx, 15, 75, 0xffffff, "Computer");
	
}

PUBLIC void reDrawComputer()
{
	extern SHEET *sht_backe;
	sheet_refresh(sht_backe, 0, 0, 90, 90);
}

PUBLIC void DrawCommand()
{
	draw_rect(10, 10+90, 80, 65+90, 0xaaaaaa);//电脑图标
	draw_rect(15, 20+90, 75, 60+90, 0x000000);
	putstr_atbuf(buf_back, binfo->scrnx, 15, 20+90, 0xffffff, ">");
	putstr_atbuf(buf_back, binfo->scrnx, 18, 74+90, 0x000000, "Command");
	putstr_atbuf(buf_back, binfo->scrnx, 19, 75+90, 0xffffff, "Command");
	
}

PUBLIC void reDrawCommand()
{
	extern SHEET *sht_backe;
	sheet_refresh(sht_backe, 0, 90, 90, 180);
}

PUBLIC void BetaWaterMark()
{
	putstr_atbuf(buf_back, binfo->scrnx, binfo->scrnx-(8*26)-16, binfo->scrny-20-(15*3), 0xffffff, "XINGJI Stdios Confidential");
	putstr_atbuf(buf_back, binfo->scrnx, binfo->scrnx-(8*31), binfo->scrny-12-(15*2), 0xffffff, "Prohibit leakage of source code");
	putstr_atbuf(buf_back, binfo->scrnx, binfo->scrnx-(8*18), binfo->scrny-12-(15*1), 0xffffff, "XJ380 OS Beta Version");
}

//哎空姐！
//先生您好请问有什么需要吗？
//能开一下窗户吗，或者换个位置吗？我妈想吐！
//哎哟我的天哪，没坐过飞机吗？
//还打开窗户啊？！出了事你负责啊？！
//那请问您是需要升舱吗？升舱费用是1920人民币。
//不，不了吧。
//升，升舱的钱我来出。
//这可不行！
//兄弟，把你的手机给我。
//拿好了。
//十五万？这哪来的钱？
//兄弟，这是你在京东网贷上的备用金，以后有急事时，可以随用随取。
//这是网贷吧，我怕！
//兄弟，每天的利息最高一块九，还没有一瓶水贵呢！
//而且新用户上新前30天免利息借贷呢！

//翻译114514次后:

//欢迎
//-嗨-嗨。
//由于一个小村庄楼上的交通和对道路的影响，对当地人产生了负面影响。晚安
//我有两只猫吗？
//这是我们的问题。我爱你。
//这张照片里有多少女孩？
//我不会死的。
//我不会死的。
//这是我们的问题。
//哈利法塔现在是世界上最大的天空。
//我不会死的。
//我今晚有两只猫？
//由于一个小村庄的交通水平和进入道路的原因，当地人口增加了一倍。
//什么非常感谢。
//由于一个小村庄的交通水平和进入道路的原因，当地人口增加了一倍。
//我太过分了。
			