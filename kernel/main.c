#include "include.h"    //使用了include\include.h头文件
#include "memory.h"
u32 buf_win[190 * 100];  //声明一个u32类型的数组
                        //大小为160*68个元素，共10880元素，每个元素可存储一个32位的值
extern PUBLIC u32 buf_cons[320 * 180];

SHEET *sht_backe;
SHEET *sht_explorer;

SHEET *sht_win;
SHEET *sht_cons;

struct FILEINFO{
	unsigned char name[8], ext[3], type;
	char reserve[10];
	unsigned short time, date, clustno;
	unsigned int size;
};

struct FILEDATA{
	char data[32];
};



void TestA()            //定义了名为TestA的无返回值的函数
{
    int i = 0;          //定义了数字类型的名为 i 的变量，并赋值为0
    while (1) {         //使用while循环，并且无限循环
                        //fifo_status是fifo.c文件里的全局变量，数据类型为int，
        if (fifo_status(&decoded_key) > 0) {                            
            printf("%c", fifo_get(&decoded_key));
            sheet_refresh(&shtctl.sheets0[0], 0, 0, binfo->scrnx, 114);
        }               //在循环中，它通过调用传入decoded_key FIFO 作为参数的 fifo_status 函数来检查 FIFO 中是否有可用的元素。
    }                   //如果有可用的元素，它会使用 fifo_get 函数检索并打印出下一个元素。然后，它通过调用 sheet_refresh 函数来更新显示。
}

void TestB()            //TestB()是一个简单的循环
{
    int i = 0x1000;     //设一个名称为i的int类型局部变量为0x1000（16进制），实际上为十进制中的4096
    while (1) {         //无限循环
        put_str("B");   //打印字符为“B”
        put_int(i++);   //变量i加1
        put_str(".");   //打印字符“.”
        milli_delay(200);
                        //上一行还包括使用milli_delay函数的200毫秒（0.2秒）的延迟
    }
}

void TestC()            //TestC()是一个简单的循环
{
    int i = 0x2000;     //设一个名称为i的int类型局部变量为0x2000（16进制），实际上为十进制中的8192
    while (1) {         //while (1)为无限循环
        put_str("C");   //打印字符为“C”
        put_int(i++);   //变量i加1
        put_str(".");   //打印字符英文句号“.”
        milli_delay(200);
                        //使用milli_delay函数进行200毫秒，即0.2秒的等待或延迟
    }
}

PRIVATE void StartAni(SHEET *sht_back)
{
    int x = binfo->scrnx;
    int y = binfo->scrny;
    char *str2 = "Copyright(C) XINGJI Studios 2017-2023";
    int str2len = strlen(str2);
    draw_rect(0, 0, x, y, 0x000000);
    putstr_atbuf(buf_back, x, 8, y - 34, 0xffffff, "Starting XJ380...");
    putstr_atbuf(buf_back, x, x - 8 - (str2len * 8), y - 34, 0xffffff, str2);
    DrawLOGO(x / 2 - 120, 114, 10, 0x000000);
	for (int i = 8; i < x - 8; i += 8)
	{
		draw_rect(8, y - 18, i, y - 8, 0x00ff00);
        sheet_refresh(sht_back, 0, 0, x, y);
	}	 
}


PUBLIC void UpExplorer()
{
	sheet_updown(sht_win, 2);
}

PUBLIC void UpCommand()
{
	sheet_updown(sht_cons, 2);
}
memory *m;

PUBLIC int kernel_main()//此为内核主函数，返回值为int类型
{
    m = memory_init(0x015a0400,240000);
    //此代码片段是初始化操作系统和GUI系统中的图形工作表（以下称为“工作表”）
    shtctl_init(&shtctl);                   //这行代码初始化用于管理工作表的shtctl结构
    SHEET *sht_back = sheet_alloc(&shtctl); //使用sheet_alloc函数分配工作表，并将其分配给指针sht_back
    sht_win  = sheet_alloc(&shtctl); //使用sheet_alloc函数分配另一个工作表，并将其分配给指针sht_win
    sht_cons = sheet_alloc(&shtctl); 
    sheet_setbuf(sht_back, buf_back, binfo->scrnx, binfo->scrny, -1);
	sht_backe = sht_back;//将局部变量放入全局变量，供其它代码使用
	sht_explorer = sht_win;
    //使用sheet_setbuf函数设置图纸sht_back的缓冲区、尺寸和透明度颜色；buf_back是缓冲区数组，binfo->scrnx和binfo->scrny是屏幕尺寸，-1表示没有透明度
    sheet_setbuf(sht_win, buf_win, 190, 100, 99);
    //使用sheet_setbuf函数设置sht_back缓冲区数组为buf_win，将buf_win数组设置为尺寸为160x68像素的缓冲区，透明度颜色设置为99。
    sheet_setbuf(sht_cons, buf_cons, 320, 180, 99);
	
	sheet_slide(sht_back, 0, 0);            //使用sheet_slide函数将sht_back图纸移动到屏幕上的位置（0，0）   
    sheet_updown(sht_back, 0);              //使用sheet_updown函数将sht_back工作表带到底层。

    StartAni(sht_back);  //启动动画

	sheet_refresh(&shtctl.sheets0[0], 0, 0, binfo->scrnx, binfo->scrny);//刷新屏幕

    init_screen();      //通过执行一些图形操作来初始化屏幕
    put_int(mem_alloc(m,0x1000));
    printf("\n");
    put_int(mem_alloc(m,0x1000));
    printf("\n");
    put_int(mem_alloc(m,0x1000));
    printf("\n");
    put_int(mem_alloc(m,0x1000));
    printf("\n");
    //窗口
	int adder=0x010000;
	char p[3];
	p[0] = (char*) adder;
    make_window(buf_win, 190, 100, "ExplTest", 99);
	int DIRADR=0x010000;
    struct FILEINFO *finfo = (struct FILEINFO *) (DIRADR);
	int fileloop=0;
	putstr_atbuf(buf_win, 190, 0, 20, 0x000000, "NAME    EXT");
	while(finfo[fileloop].name[0] != 0x00)
	{
		putstr_atbuf(buf_win, 190, 0, 35+fileloop*12, 0x000000, finfo[fileloop].name);
		fileloop++;
	}

    

    sheet_slide(sht_back, 0, 0);            //使用sheet_slide函数将sht_back图纸移动到屏幕上的位置（0，0）   
    sheet_updown(sht_back, 0);              //使用sheet_updown函数将sht_back工作表带到底层。
	
   
	
    sheet_slide(sht_cons, 40, 36);
    sheet_updown(sht_cons, 1);
    sheet_slide(sht_win, 300, 300);           //将sht_win表移动到屏幕上的位置（80，72）
    sheet_updown(sht_win, 2);               //将sht_win表带到顶层

    sheet_refresh(sht_back, 0, 0, binfo->scrnx, 48);
    //刷新屏幕上sht_back表的一部分，使其可见

    TASK    *p_task       = task_table;     //声明一个指针p_task，并使用task_table指针的值对其进行初始化；说明task_table是TASK结构的数组。
    PROCESS *p_proc       = proc_table;     //声明一个指针p_proc，并使用proc_table指针的值对其进行初始化；说明proc_table是一个PROCESS结构数组。
    TASK    *p_tcon       = task_table;
    PROCESS *p_cons       = proc_table;
    char    *p_task_stack = task_stack + STACK_SIZE_TOTAL;
    //声明一个指针p_task_stack，并使用task_stack加stack_SIZE_OTAL的值对其进行初始化；task_stack是指向堆栈开始的指针，stack_SIZE_TOAL是堆栈的总大小。
    u16      selector_ldt = SELECTOR_LDT_FIRST;
    //声明一个16位无符号整数selector_ldt，并使用selector_ldt_FIRST值对其进行初始化；大概SELECTOR_LDT_FIRST是表示LDT（本地描述符表）选择器的特定值的宏或常量。

    //这个代码片段是一个循环，它迭代系统中的任务和进程，并设置它们各自的属性
    int i;                                  //声明了一个int类型的名称为i的变量
    for (i = 0; i < NR_TASKS; i++) {        //启动一个从0迭代到NR_TASKS-1的循环。
    	
    	/*任务A*/
        strcpy(p_proc->p_name, p_task->name);
        //将当前任务的名称（p_task->name）复制到当前进程的名称（sp_proc->p_name）
        p_proc->pid = i;                    //将当前进程的进程ID（pid）设置为i的值

        p_proc->ldt_sel = selector_ldt;     //将当前进程的本地描述符表（LDT）选择器设置为selector_LDT的值
        
        memcpy(&p_proc->ldts[0], &gdt[SELECTOR_KERNEL_CS >> 3], sizeof(DESCRIPTOR));
        //将段描述符从全局描述符表（GDT）复制到当前进程的本地描述符表（LDT）
        p_proc->ldts[0].attr1 = DA_C | PRIVILEGE_USER << 5;
        //将LDT中代码段描述符的属性设置为具有代码特权级别USER（3）和可执行段
        memcpy(&p_proc->ldts[1], &gdt[SELECTOR_KERNEL_DS >> 3], sizeof(DESCRIPTOR));
        //将段描述符从GDT复制到当前进程的LDT
        p_proc->ldts[1].attr1 = DA_DRW | PRIVILEGE_USER << 5;
        //将LDT中数据段描述符的属性设置为具有数据特权级别USER（3）和读/写访问权限

        //接下来的几行将当前进程的段寄存器（cs、ds、es、fs、ss和gs）的值设置为基于当前任务和特权级别的适当值
        p_proc->regs.cs = ((0 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_proc->regs.ds = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_proc->regs.es = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_proc->regs.fs = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_proc->regs.ss = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_proc->regs.gs = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;

        p_proc->regs.eip = (u32) p_task->initial_eip;
        //将当前进程的指令指针（eip）寄存器设置为当前任务的初始入口点
        p_proc->regs.esp = (u32) p_task_stack;
        //将当前进程的堆栈指针（esp）寄存器设置为p_task_stack的当前值
        p_proc->regs.eflags = 0x1202;
        //将当前进程的标志（eflags）寄存器设置为值0x1202（十六进制，十进制4610）

        p_task_stack -= p_task->stacksize;  //递减p_task_stack指针，为下一个任务的堆栈保留空间
        p_proc++;                           //增加p_proc指针以指向proc_table数组中的下一个进程
        p_task++;                           //增加p_task指针指向task_table数组中的下一个任务
        selector_ldt += 1 << 3;             //将selector_ldt值增加1，左移3（相当于乘以8），以设置下一个ldt条目的选择器
        
        
        
        
        
    	/*任务B（命令行）*/
        strcpy(p_cons->p_name, p_tcon->name);
        p_cons->pid = i;                    //将当前进程的进程ID（pid）设置为i的值

        p_cons->ldt_sel = selector_ldt;     //将当前进程的本地描述符表（LDT）选择器设置为selector_LDT的值
        
        memcpy(&p_cons->ldts[0], &gdt[SELECTOR_KERNEL_CS >> 3], sizeof(DESCRIPTOR));
        //将段描述符从全局描述符表（GDT）复制到当前进程的本地描述符表（LDT）
        p_cons->ldts[0].attr1 = DA_C | PRIVILEGE_USER << 5;
        //将LDT中代码段描述符的属性设置为具有代码特权级别USER（3）和可执行段
        memcpy(&p_cons->ldts[1], &gdt[SELECTOR_KERNEL_DS >> 3], sizeof(DESCRIPTOR));
        //将段描述符从GDT复制到当前进程的LDT
        p_cons->ldts[1].attr1 = DA_DRW | PRIVILEGE_USER << 5;
        //将LDT中数据段描述符的属性设置为具有数据特权级别USER（3）和读/写访问权限

        //接下来的几行将当前进程的段寄存器（cs、ds、es、fs、ss和gs）的值设置为基于当前任务和特权级别的适当值
        p_cons->regs.cs = ((0 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_cons->regs.ds = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_cons->regs.es = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_cons->regs.fs = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_cons->regs.ss = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;
        p_cons->regs.gs = ((1 * 8) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | RPL_USER;

        p_cons->regs.eip = (u32) &start_console;
        //将当前进程的指令指针（eip）寄存器设置为当前任务的初始入口点
        *((int *) (p_cons->regs.esp + 4)) = (int) sht_cons;
        //p_cons->regs.esp = (u32) p_task_stack;
        p_cons->regs.eflags = 0x1203;
        //将当前进程的标志（eflags）寄存器设置为值0x1203（十六进制，十进制4611）

        p_task_stack -= p_tcon->stacksize;  //递减p_task_stack指针，为下一个任务的堆栈保留空间
        p_cons++;                           //增加p_proc指针以指向proc_table数组中的下一个进程
        p_tcon++;                           //增加p_task指针指向task_table数组中的下一个任务
        selector_ldt += 1 << 3;             //将selector_ldt值增加1，左移3（相当于乘以8），以设置下一个ldt条目的选择器        
    }

    proc_table[0].ticks = proc_table[0].priority = 150;
    //将第一个proc_table条目的刻度和优先级字段设置为150
    proc_table[1].ticks = proc_table[1].priority = 0;
    //将第二个proc_table条目的刻度和优先级字段设置为0
    proc_table[2].ticks = proc_table[2].priority = 0;
    //将第三个proc_table条目的刻度和优先级字段设置为0

    init_clock();                           //调用init_clock()函数来初始化系统时钟
    init_keyboard();                        //调用init_keyboard()函数来初始化键盘
    init_mouse();                           //调用init_mouse()函数来初始化鼠标
    mouse_sheet_update_to(3);
    /*
    for(u16 h = 1;h < MAX_SHEETS;h++){
        SHTCTL *check_ctl = &shtctl;
        if(check_ctl->sheets0[i].flags != 1){
            mouse_sheet_update_to(i);
            check_ctl->sheets0[i].flags = 1;
            break;
        }
    }
    */
    k_reenter = 0;                          //将用于内核重新输入的k_renter变量设置为0
    ticks = 0;                              //将用于计数时钟刻度的刻度变量设置为0

    p_proc_ready = proc_table;              //将p_proc_ready指针设置为指向proc_table数组中的第一个条目
    restart();                              //调用restart()函数，该函数执行一些内部操作并将控制权转移到下一个进程
    
    while(1)
	{
		
	}
	
}

//源神，启动！
