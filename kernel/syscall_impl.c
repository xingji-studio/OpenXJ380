#include "include.h"

PUBLIC int sys_get_ticks()
{
    return ticks;
}

PUBLIC int sys_write(char *str)
{
    put_str(str);
    return strlen(str);
}

PUBLIC int sys_writeMem(int mem,int w)
{
	return;
}