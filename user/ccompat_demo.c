#include "x3api.h"

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    xapi_Output("XJ380 C 兼容层演示\n");
    xapi_PrintLine("这个应用使用 C 编写，并直接调用 xapi_*。");
    xapi_Printf("当前 tick：%llu\n", (unsigned long long)xapi_GetTime());

    UserInfo user_info;
    xapi_GetCurrentUser(&user_info);
    xapi_Printf("当前用户：%s\n", user_info.name);

    xapi_EndLine();
    xapi_PrintLine("按任意键退出...");
    xapi_Getch();
    return 0;
}
