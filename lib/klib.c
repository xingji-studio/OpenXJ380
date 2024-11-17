#include "include.h"

PRIVATE char *tohex(char *str, int num)
{
    char *p = str;
    char ch;
    int i, flag = 0;

    *p++ = '0';
    *p++ = 'x';

    if (num == 0) *p++ = '0';
    else {
        for (i = 28; i >= 0; i -= 4) {
            ch = (num >> i) & 0xf;
            if (flag || (ch > 0)) {
                flag = 1;
                ch += '0';
                if (ch > '9') ch += 7;
                *p++ = ch;
            }
        }
    }

    *p = 0;
    return str;
}

PUBLIC void put_int(int i)
{
    char str[16];
    tohex(str, i);
    put_str(str);
}
