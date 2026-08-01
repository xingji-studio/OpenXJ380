#include <x3api.h>
#include <krlibc.h>
#include "fm_proto.h"

bool get_file_type(char *name, char *type)
{
    char *p = name;
    char *q = type;
    bool have_type;
    while (*p)
    {
        p++;
        if (*p == '.')
            have_type = true;
    }

    if (!have_type)
        return false;
    
    while (*p != '.')
        p--;

    p++;

    while (*p)
    {
        *q = *p;
        p++; q++;
    }
    
    return true;
}

void cat_path(char *folder_name)
{
    char *p = current_path;

    while (*p) p++;

    p--;

    if (*p == '/')
    {
        strcat(current_path, folder_name);
    }
    else
    {
        strcat(current_path, "/");
        strcat(current_path, folder_name);
    }
}

void revert_path(char *path)
{
    record_current_path();

    char *s = path;
    while (*s)
        s++;

    while (*s != '/')
        s--;

    if (s == path) s++;

    *s = '\0';
}

void record_current_path()
{
    if (path_p + 1 == path_r) path_r++;
    if (path_r == 20) path_r = 0;

    strcpy(path_his[path_p], current_path);
    path_p++;

    if (path_p == 20) path_p = 0;
    
    choosing_index = -1;
}
