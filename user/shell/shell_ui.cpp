#include "st_proto.h"
#include "convert.h"
#include "gh_sign.h"

#define CALC_FONT_WIDTH(str) ((strlen(str) - 1) * 10)
#define TERMINAL_CURSOR_WIDTH 8
#define TERMINAL_CURSOR_HEIGHT 16
#define TERMINAL_CURSOR_BLINK_MS 500

char arrow_head[19][12] = {
    "#########",
    ".########",
    "..#######",
    "...######",
    "....#####",
    ".....####",
    "......###",
    ".......##",
    "........#",
    ".........",
    "........#",
    ".......##",
    "......###",
    ".....####",
    "....#####",
    "...######",
    "..#######",
    ".########",
    "#########"
};

/*
 __  __   _ _____  ___   ___     ___  ____  
 \ \/ /  | |___ / ( _ ) / _ \   / _ \/ ___| 
  \  /_  | | |_ \ / _ \| | | | | | | \___ \ 
  /  \ |_| |___) | (_) | |_| | | |_| |___) |
 /_/\_\___/|____/ \___/ \___/   \___/|____/ 

 */
char info_logo[6][45] = {
    " __  __   _ _____  ___   ___     ___  ____  ",
    " \\ \\/ /  | |___ / ( _ ) / _ \\   / _ \\/ ___| ",
    "  \\  /_  | | |_ \\ / _ \\| | | | | | | \\___ \\ ",
    "  /  \\ |_| |___) | (_) | |_| | | |_| |___) |",
    " /_/\\_\\___/|____/ \\___/ \\___/   \\___/|____/ ",
    " ",
};

static bool terminal_cursor_visible = false;
static bool terminal_cursor_enabled = true;
static uint64_t terminal_cursor_elapsed_ms = 0;

void hide_terminal_cursor(bool refresh)
{
    if (!terminal_cursor_visible) return;

    xapi_DrawRect(handle, cur_x, cur_y, cur_x + TERMINAL_CURSOR_WIDTH, cur_y + TERMINAL_CURSOR_HEIGHT,
                  BACKGROUND_COLOR, true);
    terminal_cursor_visible = false;

    if (refresh) { xapi_RefreshWindow(handle); }
}

void show_terminal_cursor(bool refresh)
{
    if (!terminal_cursor_enabled || paint_cursor_lock || terminal_cursor_visible) return;

    xapi_DrawRect(handle, cur_x, cur_y, cur_x + TERMINAL_CURSOR_WIDTH, cur_y + TERMINAL_CURSOR_HEIGHT,
                  TERMINAL_TEXT_COLOR, true);
    terminal_cursor_visible = true;

    if (refresh) { xapi_RefreshWindow(handle); }
}

void set_terminal_cursor_enabled(bool enabled, bool refresh)
{
    terminal_cursor_enabled = enabled;
    if (!enabled) { hide_terminal_cursor(refresh); }
    else if (refresh)
    {
        reset_terminal_cursor_blink();
        show_terminal_cursor(true);
    }
}

void reset_terminal_cursor_blink()
{
    terminal_cursor_elapsed_ms = 0;
}

void update_terminal_cursor_blink(uint64_t elapsed_ms)
{
    if (!terminal_cursor_enabled || paint_cursor_lock) return;

    terminal_cursor_elapsed_ms += elapsed_ms;
    if (terminal_cursor_elapsed_ms < TERMINAL_CURSOR_BLINK_MS) return;

    terminal_cursor_elapsed_ms = 0;
    if (terminal_cursor_visible) { hide_terminal_cursor(true); }
    else
    {
        show_terminal_cursor(true);
    }
}

void paint_arrow_sign(int sx, int sy)
{
    hide_terminal_cursor(false);
    front_line_is_arrow = true;
    UserInfo current_user;
    xapi_GetCurrentUser(&current_user);
    
    xapi_DrawRect(handle, sx, sy, sx + CALC_FONT_WIDTH(current_path) + 13 + 2, sy + 18, 0x00a2e8ff, true);
    xapi_DrawRect(handle, sx + CALC_FONT_WIDTH(current_path) + 27 + 2, sy,
                  sx + CALC_FONT_WIDTH(current_path) + 27 + 2 +
                      CALC_FONT_WIDTH(user_type_convert_table[current_user.user_type]) + 13,
                  sy + 18, 0xfff200ff, true);

    for (int y = 0; y < 19; y++)
    {
        for (int x = 0; x < 11; x++)
        {
            if (arrow_head[y][x] == '#')
            {
                xapi_DrawPoint(handle, x + sx + CALC_FONT_WIDTH(current_path) + 19 + 2, y + sy, 0xfff200ff);
            }
        }
    }

    for (int y = 0; y < 19; y++)
    {
        for (int x = 0; x < 11; x++)
        {
            if (arrow_head[y][x] == '.')
            {
                xapi_DrawPoint(handle,
                               x + sx + CALC_FONT_WIDTH(current_path) + 27 + 2 +
                                   CALC_FONT_WIDTH(user_type_convert_table[current_user.user_type]) + 14,
                               y + sy, 0xfff200ff);
            }
        }
    }

    for (int y = 0; y < 19; y++)
    {
        for (int x = 0; x < 11; x++)
        {
            if (arrow_head[y][x] == '.')
            {
                xapi_DrawPoint(handle, x + sx + CALC_FONT_WIDTH(current_path) + 14 + 2, y + sy, 0x00a2e8ff);
            }
        }
    }

    xapi_DrawSWText(handle, sx + 5, sy + 2, current_path, 0xffffffff);
    xapi_DrawSWText(handle, sx + CALC_FONT_WIDTH(current_path) + 34, sy + 2,
                    user_type_convert_table[current_user.user_type], 0x000000ff);

    cur_x = sx + CALC_FONT_WIDTH(current_path) + 27 + CALC_FONT_WIDTH(user_type_convert_table[current_user.user_type]) + 29;
    cur_y = sy + 4;

    if (cur_y >= TML_Y - 32) scroll();

    reset_terminal_cursor_blink();
    show_terminal_cursor(false);
    xapi_RefreshWindow(handle);
}

void clear_screen()
{
    hide_terminal_cursor(false);
    xapi_DrawRect(handle, 0, 0, TML_X - 1, TML_Y - 1, BACKGROUND_COLOR, true);
    cur_x = 8;
    cur_y = 4;
    xapi_RefreshWindow(handle);
}

void print_to_console(char *str)
{
    hide_terminal_cursor(false);
    xapi_DrawSWText(handle, *ab_x, *ab_y, str, TERMINAL_TEXT_COLOR);
    newline();
}

void print_to_console_zh(char *str)
{
    hide_terminal_cursor(false);
    xapi_DrawSWText(handle, *ab_x, *ab_y, str, TERMINAL_TEXT_COLOR);
    newline();
}

void putchar_at_cur(char *ch)
{
    if (p_chbuffer < 255)
    {
        char_buffer[p_chbuffer++] = *ch;
    }
    else
    {
        flush_buffer();
        if (p_chbuffer < 255) { char_buffer[p_chbuffer++] = *ch; }
    }
}

void newline()
{
    hide_terminal_cursor(false);
    cur_x                = 8;
    cur_y               += front_line_is_arrow ? 20 : 16;
    front_line_is_arrow  = false;
    if (cur_y >= TML_Y - 32) scroll();
}

void backspace()
{
    if (p_inbuf == 0) return;
    hide_terminal_cursor(false);
    if (cur_x != 8) { cur_x -= 9; }
    else
    {
        cur_x  = TML_X - 17;
        cur_y -= 16;
    }
    xapi_DrawRect(handle, cur_x, cur_y, cur_x + 9, cur_y + 16, BACKGROUND_COLOR, true);
    reset_terminal_cursor_blink();
    show_terminal_cursor(false);
    xapi_RefreshWindow(handle);
    p_inbuf--;
}

void flush_buffer()
{
    while (true)
    {
        if (!command_ck_lock) break;
    }

    if (p_chbuffer > 0)
    {
        char_buffer[p_chbuffer] = '\0';
        char temp[256];
        strcpy(temp, char_buffer);
        int temp_pchb = p_chbuffer;

        hide_terminal_cursor(false);
        if (cur_x + temp_pchb * 9 >= TML_X - 8) { newline(); }

        p_chbuffer = 0;
        xapi_DrawSWText(handle, cur_x, cur_y, temp, TERMINAL_TEXT_COLOR);

        strcpy(&input_buf[p_inbuf], char_buffer);
        p_inbuf += strlen(char_buffer);
        cur_x += temp_pchb * 9;
        reset_terminal_cursor_blink();
        show_terminal_cursor(false);
        xapi_RefreshWindow(handle);
    }
}

void scroll()
{
    hide_terminal_cursor(false);
    XCOLOR winbuf[TML_X * TML_Y];
    xapi_ReadBuffer(handle, 0, 0, TML_X, TML_Y, winbuf);
    xapi_DrawRect(handle, 0, 0, TML_X - 1, TML_Y - 1, BACKGROUND_COLOR, true);
    xapi_WriteBuffer(handle, 0, 0, TML_X, TML_Y - 16, winbuf + TML_X * 16);
    cur_y -= 16;
}
