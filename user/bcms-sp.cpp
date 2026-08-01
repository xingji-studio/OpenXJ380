#include "./xapi/include/x3api.h"

static int bcms_sp_main_impl(int argc, char *argv[], char *envp[])
{
    xapi_Printf("%s 哈机密\n","gulu");
    char t[114514];
    xapi_Input(t);
    xapi_Output("你好，世界！");
    xapi_Output(t);
    while (1);
    return 0;
}

extern "C" int bcms_sp_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int bcms_sp_main_cpp(int argc, char *argv[], char *envp[])
{
    return bcms_sp_main_impl(argc, argv, envp);
}
