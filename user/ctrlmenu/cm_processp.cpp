#include <x3api.h>
#include <krlibc.h>
#include "cm_proto.h"

void process_left_key(int x, int y)
{
    if (y > 60 && y < 120)
    {
        if (x > 280 && x < 386) { cindex = 1; }
        else if (x > 386 && x < 544) { cindex = 2; }
        else if (x > 544 && x < 670) { cindex = 3; }
        if (cindex != 3) ctrlmenu_settings_hide_controls();
        draw_background();
    }
    if (cindex == 1)
    {
        ctrlmenu_handle_mainpage_click(x, y);
    }
    else if (cindex == 2)
    {
        ctrlmenu_handle_app_showcase_click(x, y);
    }
    else if (cindex == 3)
    {
        ctrlmenu_settings_handle_click(x, y);
    }
}
