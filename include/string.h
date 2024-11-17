#ifndef _XJ380_STRING_H_
#define _XJ380_STRING_H_

PUBLIC void *memcpy(void *dest, const void *src, int size);
PUBLIC void memset(void *dest, u8 value, u32 size);
PUBLIC char *strcpy(char *dest, const char *src);
PUBLIC u32 strlen(const char *str);
PUBLIC u32 strcmp(char *from_str, char *cmp_str);

#endif