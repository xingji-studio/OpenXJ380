#include <x3api.h>
#include <krlibc.h>
#include "cm_proto.h"

HDLE handle;

UINT64 win_width;
UINT64 win_height;

bool exit_cm = false;

void ctrlmenu_MessagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    switch (Type)
    {
    case MSG_LBUTTON:
        process_left_key(hData, lData);
        break;
    case MSG_ROLLER:
        ctrlmenu_settings_scroll((int)hData);
        break;
    case MSG_CRL:
        if (cindex == 3 && ctrlmenu_settings_handle_control(hData, lData)) break;
        if (hData <= 1999) { change_setting_apps(hData); }
        else if (hData == 2001) { change_clock_hour_offset(+1); }
        else if (hData == 2002) { change_clock_hour_offset(23); }
        else if (hData == 3000) { change_setting_background(); }
        else if (hData == 5000) { change_settings_language(XJ380_LANGUAGE_ZH_CN); }
        else if (hData == 5001) { change_settings_language(XJ380_LANGUAGE_EN_US); }
        else if (hData == 114514) { delete_input_box(false); }
        else if (hData == 114515) { delete_input_box(true); }
        break;
    }
    return;
}

static int ctrlmenu_main_impl(int argc, char *argv[], char *envp[])
{
    (void)envp;

    if (argc == 1)
    {
        if (strcmp(argv[0], "shortdock-open-settings") == 0)
        {
            cindex = 3;
        }
        else if (strcmp(argv[0], "shortdock-open-about") == 0)
        {
            xapi_Run((char *)"/apps/system/xjver.elf");
            return 0;
        }
        else if (strcmp(argv[0], "shortdock-open-graphics") == 0)
        {
            cindex = 3;
            setting_cindex = 3;
        }
    }

    XWINDOW Winfo;
    Winfo.title = xj380_tr("控制中心", "Control Center");
    Winfo.sets  = XWIN_FULL_SCR;  
    xapi_CreateWindow(&handle, &Winfo);
    xapi_GetWindowSize(handle, &win_width, &win_height);
    init_ctrlmenu_background_cache();
    ctrlmenu_settings_init();
    SetMsgPrcor(handle, ctrlmenu_MessagePrcor);

    draw_background();

    int old_min = 60;
    int old_sec = -1;

    while (1)
    {
        if (exit_cm)
            xapi_Exit(0);
            
        xapi_Sleep(1);

        TimeType tm;
        xapi_GetTimeX(&tm);

        if (tm.tm_min != old_min)
        {
            draw_background_time();
            if (cindex == 3 && setting_cindex == 5)
            {
                draw_background_body();
            }
            old_min = tm.tm_min;
        }
        else if (cindex == 3 && setting_cindex == 2 && tm.tm_sec != old_sec)
        {
            draw_background_body();
        }
        old_sec = tm.tm_sec;

        xapi_Sleep(1000);
    }
}

extern "C" int ctrlmenu_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int ctrlmenu_main_cpp(int argc, char *argv[], char *envp[])
{
    return ctrlmenu_main_impl(argc, argv, envp);
}
