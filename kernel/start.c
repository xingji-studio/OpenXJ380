#include "include.h"
//初始化
PUBLIC void cstart()
{
    init_gdt();
    init_idt();
}