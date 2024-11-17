#include "include.h"

PUBLIC void *memcpy(void *dest, const void *src, int size)
{
    //assert(dest != NULL && src != NULL);
    u8 *dst = (u8 *) dest;
    u8 *source = (u8 *) src;
    while (size-- > 0) *dst++ = *source++;
    return dest;
}

PUBLIC void memset(void *dest, u8 value, u32 size)
{
    //assert(dest != NULL);
    u8 *dst = (u8 *) dest;
    while (size-- > 0) *dst++ = value;
}

PUBLIC char *strcpy(char *dest, const char *src)
{
    //assert(dest != NULL && src != NULL);
    char *r = dest;
    while ((*dest++ = *src++));
    return r;
}

PUBLIC u32 strlen(const char *str)
{
    //assert(str != NULL);
    const char *p = str;
    while (*p++);
    return p - str - 1;
}

PUBLIC u32 strcmp(char *from_str, char *cmp_str)
{
	while ((*from_str != '\0') && (*from_str == *cmp_str))
	{
		from_str++;
		cmp_str++;
	}
	return *from_str - *cmp_str;
}
