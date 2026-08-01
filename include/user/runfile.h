#pragma once

#include <stdint.h>

struct RunfileSettings_Item
{
    char exname[10];
    char describe[128];  // 描述
    char runpath[256];  // 打开方式
};

struct RunfileSettings_Format
{
    RunfileSettings_Item items[1024];
};

void init_runfile();
bool init_user_dirs(const char *username);
bool init_user_runfile(const char *username);
bool init_user_settings(const char *username);
bool init_user_profile(const char *username);

void runfile(char *path);
bool get_file_exname(char *name, char *exname);
