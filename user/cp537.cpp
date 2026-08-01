#include "./xapi/include/x3api.h"
// cp537, start!

char buf[400 * 160];

#define s ((((((((((((((((0
#define v )*2
#define _ )*2+1
#define e

int uparrow[16] = {
    s v v v v v v v v v v v v v v v v e, s v _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ e, s v v v v v v v v v v v v v v v v e,
    s v v v v v v v v _ v v v v v v v e, s v v v v v v _ _ _ _ _ v v v v v e, s v v v v _ _ _ _ _ _ _ _ _ v v v e,
    s v v v _ _ _ _ _ _ _ _ _ _ _ v v e, s v _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ e, s v v v v v v _ _ _ _ _ v v v v v e,
    s v v v v v v _ _ _ _ _ v v v v v e, s v v v v v v _ _ _ _ _ v v v v v e, s v v v v v v _ _ _ _ _ v v v v v e,
    s v v v v v v _ _ _ _ _ v v v v v e, s v v v v v v _ _ _ _ _ v v v v v e, s v v v v v v v v v v v v v v v v e};

#undef s
#undef v
#undef _
#undef e

void draw_uparrow(HDLE handle, int x, int y)
{
    for (int j = 15; j >= 0; j--)
    {
        for (int i = 0; i < 16; i++)
        {
            if ((uparrow[i] >> j) & 1) xapi_DrawPoint(handle, x + j, y + i, 0x000000ff);
        }
    }
}

void cp537_MessagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    switch (Type)
    {
    case MSG_CRL:
        xapi_OutputSerial("114514\n");
        break;
    }
    return;
}

static int cp537_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;
    write(1, (char *)"cp357\n", 0);
    XWINDOW window;
    window.width  = 400;
    window.height = 160;
    window.title  = (char *)"CP537 演示";
    // window.sets = NORMAL;
    HDLE handle;
    xapi_CreateWindow(&handle, &window);
    SetMsgPrcor(handle, cp537_MessagePrcor);

    xapi_Button(handle, 0, 0, 0, "测试");

    // 以下颜色用的都是ARGB，不知道用不用调
    xapi_DrawText(handle, 10, 27, (char *)"山地奥斯卡537 2023/8/14 20:35:14", 10, 0x45459eff);
    xapi_DrawRect(handle, 26, 48, 382, 106, 0xc6c6c6ff, true);
    xapi_DrawText(handle, 34, 47, (char *)"Hmtc 2023/8/14 20:34:37", 10, 0x000000ff);
    xapi_DrawText(handle, 34, 65, (char *)"@山地奥斯卡537 另外，不是", 10, 0x000000ff);
    xapi_DrawText(handle, 34, 83, (char *)"utf-8吗？", 10, 0x000000ff);
    xapi_DrawRect(handle, 26, 107, 76, 126, 0x92bbffff, true); // @Hmtc长度5*8=40，此处沿用旧设定，如果不对需要改
    xapi_DrawText(handle, 28, 106, (char *)"@Hmtc", 10, 0x000000ff);
    xapi_DrawText(handle, 80, 106, (char *)"老兄，中文不支持UTF-8，因为中文是双字符", 10, 0x000000ff);
    draw_uparrow(handle, 365, 50);
    xapi_RefreshWindow(handle);
    // xapi_CloseWindow(handle);
    execve((char *)"/shell.elf", NULL, NULL);
    return 0;
}

extern "C" int cp537_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int cp537_main_cpp(int argc, char *argv[], char *envp[])
{
    return cp537_main_impl(argc, argv, envp);
}

// HariMain原版见下
/*
void HariMain(void)
{
    int win;
    // 创建窗口
    win = api_openwin(buf, 400, 160, -1, "CP537 演示程序");
    // 开始写字
    api_putstrwin(win, 10, 30, 4, 114, "山地奥斯卡537 2023/8/14 20:35:14");
    api_boxfilwin(win, 26, 48, 382, 106, 8);
    api_putstrwin(win, 34, 50, 0, 114, "Hmtc 2023/8/14 20:34:37");
    api_putstrwin(win, 34, 68, 0, 114, "@山地奥斯卡537 另外，不是");
    api_putstrwin(win, 34, 86, 0, 114, "utf-8吗？");
    api_boxfilwin(win, 26, 107, 66, 124, 16); // @Hmtc长度5*8=40
    api_putstrwin(win, 28, 109, 0, 114, "@Hmtc");
    api_putstrwin(win, 72, 109, 0, 114, "老兄，中文不支持UTF-8，因为中文是双字符");
    draw_uparrow(buf, 350, 50);
    for (;;)
    {
        if (api_getkey(1) == 0x0a) // 等待回车结束
        {
            break;
        }
    }
    // 关闭窗口
    api_closewin(win);
    api_end();
}

void draw_uparrow(char *winbuf, int x, int y)
{
    char *pointer = winbuf + y * 400 + x;
    int i;
    for (i = 0; i < 16; i++) {
        int pixels = uparrow[i];
        if (pixels & 0x8000) pointer[0] = 0;
        if (pixels & 0x4000) pointer[1] = 0;
        if (pixels & 0x2000) pointer[2] = 0;
        if (pixels & 0x1000) pointer[3] = 0;
        if (pixels & 0x0800) pointer[4] = 0;
        if (pixels & 0x0400) pointer[5] = 0;
        if (pixels & 0x0200) pointer[6] = 0;
        if (pixels & 0x0100) pointer[7] = 0;
        if (pixels & 0x0080) pointer[8] = 0;
        if (pixels & 0x0040) pointer[9] = 0;
        if (pixels & 0x0020) pointer[10] = 0;
        if (pixels & 0x0010) pointer[11] = 0;
        if (pixels & 0x0008) pointer[12] = 0;
        if (pixels & 0x0004) pointer[13] = 0;
        if (pixels & 0x0002) pointer[14] = 0;
        if (pixels & 0x0001) pointer[15] = 0;
        pointer += 400;
    }
}
*/
