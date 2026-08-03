#include "../xapi/include/krlibc.h"
#include "../xapi/include/x3api.h"
#include "convert.h"
#include "st_proto.h"

extern int  cur_x;
extern int  cur_y;
extern int *ab_x;
extern int *ab_y;
extern char char_buffer[64];
extern int  p_chbuffer;
extern bool command_ck_lock;

enum TerminalKeyCode
{
    TERM_KEY_ESC = 128,
    TERM_KEY_BACKSPACE,
    TERM_KEY_TAB = 130,
    TERM_KEY_ENTER,
    TERM_KEY_CAPS,
    TERM_KEY_SHIFT,
    TERM_KEY_CTRL,
    TERM_KEY_ALT,
    TERM_KEY_SPACE,
    TERM_KEY_F1 = 136,
    TERM_KEY_F2,
    TERM_KEY_F3,
    TERM_KEY_F4,
    TERM_KEY_F5,
    TERM_KEY_F6,
    TERM_KEY_F7,
    TERM_KEY_F8,
    TERM_KEY_F9,
    TERM_KEY_F10,
    TERM_KEY_F11,
    TERM_KEY_F12,
    TERM_KEY_NUM_LOCK,
    TERM_KEY_SCROLL_LOCK,
    TERM_KEY_HOME,
    TERM_KEY_UP,
    TERM_KEY_PAGE_UP,
    TERM_KEY_LEFT,
    TERM_KEY_RIGHT,
    TERM_KEY_END,
    TERM_KEY_DOWN,
    TERM_KEY_PAGE_DOWN,
    TERM_KEY_INSERT,
    TERM_KEY_DELETE,
};

char app_input_buffer[1024];
int  p_input_buffer = 0;
bool input_is_enter = false;

void terminal_child_putchar(char ch)
{
    if (p_input_buffer >= (int)sizeof(app_input_buffer) - 2) return;

    char text[2] = {ch, '\0'};
    app_input_buffer[p_input_buffer++] = ch;
    app_input_buffer[p_input_buffer] = '\0';
    if (!terminal_input_no_echo()) ptt_console_sp(text);
    reset_terminal_cursor_blink();
    show_terminal_cursor(false);
    xapi_RefreshWindow(handle);
}

bool terminal_child_special_key(uint64_t key)
{
    const char *seq = NULL;
    switch (key)
    {
    case TERM_KEY_ESC: seq = "\x1b"; break;
    case TERM_KEY_TAB: seq = "\t"; break;
    case TERM_KEY_F1: seq = "\x1bOP"; break;
    case TERM_KEY_F2: seq = "\x1bOQ"; break;
    case TERM_KEY_F3: seq = "\x1bOR"; break;
    case TERM_KEY_F4: seq = "\x1bOS"; break;
    case TERM_KEY_F5: seq = "\x1b[15~"; break;
    case TERM_KEY_F6: seq = "\x1b[17~"; break;
    case TERM_KEY_F7: seq = "\x1b[18~"; break;
    case TERM_KEY_F8: seq = "\x1b[19~"; break;
    case TERM_KEY_F9: seq = "\x1b[20~"; break;
    case TERM_KEY_F10: seq = "\x1b[21~"; break;
    case TERM_KEY_F11: seq = "\x1b[23~"; break;
    case TERM_KEY_F12: seq = "\x1b[24~"; break;
    case TERM_KEY_HOME: seq = "\x1b[H"; break;
    case TERM_KEY_UP: seq = "\x1b[A"; break;
    case TERM_KEY_PAGE_UP: seq = "\x1b[5~"; break;
    case TERM_KEY_LEFT: seq = "\x1b[D"; break;
    case TERM_KEY_RIGHT: seq = "\x1b[C"; break;
    case TERM_KEY_END: seq = "\x1b[F"; break;
    case TERM_KEY_DOWN: seq = "\x1b[B"; break;
    case TERM_KEY_PAGE_DOWN: seq = "\x1b[6~"; break;
    case TERM_KEY_INSERT: seq = "\x1b[2~"; break;
    case TERM_KEY_DELETE: seq = "\x1b[3~"; break;
    default: return false;
    }

    int len = (int)strlen(seq);
    if (p_input_buffer + len >= (int)sizeof(app_input_buffer) - 1) return true;
    strcpy(&app_input_buffer[p_input_buffer], seq);
    p_input_buffer += len;
    input_is_enter = true;
    terminal_child_flush_input_if_requested();
    return true;
}

void terminal_child_backspace()
{
    if (p_input_buffer <= 0) return;

    p_input_buffer--;
    app_input_buffer[p_input_buffer] = '\0';

    if (terminal_input_no_echo()) return;

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
}

void terminal_child_submit_line()
{
    if (p_input_buffer < (int)sizeof(app_input_buffer) - 1)
    {
        app_input_buffer[p_input_buffer++] = '\n';
        app_input_buffer[p_input_buffer] = '\0';
    }
    newline();
    input_is_enter = true;
    terminal_child_flush_input_if_requested();
    reset_terminal_cursor_blink();
    show_terminal_cursor(true);
    xapi_RefreshWindow(handle);
}

void terminal_child_flush_input_if_requested()
{
    if (!check_read_xttp_buffer() || !input_is_enter) return;

    input_is_enter = false;
    write_xttp_buffer(app_input_buffer);
    app_input_buffer[0] = '\0';
    p_input_buffer      = 0;
    reset_terminal_cursor_blink();
    show_terminal_cursor(true);
}

void tam_flush_buffer()
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
        bool no_echo = terminal_input_no_echo();

        hide_terminal_cursor(false);
        if (!no_echo && cur_x + temp_pchb * 9 >= TML_X - 8) { newline(); }

        p_chbuffer = 0;
        if (!no_echo) xapi_DrawSWText(handle, cur_x, cur_y, temp, TERMINAL_TEXT_COLOR);

        strcpy(&app_input_buffer[p_input_buffer], char_buffer);
        p_input_buffer += strlen(char_buffer);
        if (!no_echo) cur_x += temp_pchb * 9;
        reset_terminal_cursor_blink();
        show_terminal_cursor(false);
        xapi_RefreshWindow(handle);
    }
}

#include <stdint.h>

#define TERM_LEFT_X      8
#define TERM_TOP_Y       4
#define TERM_RIGHT_X     (TML_X - 8)
#define TERM_BOTTOM_Y    (TML_Y - 32)
#define TERM_CELL_WIDTH  9
#define TERM_CELL_HEIGHT 16
#define TERM_COLS        ((TERM_RIGHT_X - TERM_LEFT_X) / TERM_CELL_WIDTH)
#define TERM_ROWS        ((TERM_BOTTOM_Y - TERM_TOP_Y) / TERM_CELL_HEIGHT)
#define TERM_SEQ_MAX     256
#define TERM_PARAM_MAX   32

extern char app_input_buffer[1024];
extern int  p_input_buffer;
extern bool input_is_enter;

static XCOLOR terminal_pixel_buffer[TML_X * TML_Y];

static UINT32 ansi_color_table[16] = {
    0x000000ff, 0xaa0000ff, 0x00aa00ff, 0xaa5500ff,
    0x0000aaff, 0xaa00aaff, 0x00aaaaff, 0xaaaaaaff,
    0x555555ff, 0xff5555ff, 0x55ff55ff, 0xffff55ff,
    0x5555ffff, 0xff55ffff, 0x55ffffff, 0xffffffff,
};

struct TerminalStyle
{
    UINT32 fg;
    UINT32 bg;
    bool bold;
    bool faint;
    bool underline;
    bool inverse;
    bool conceal;
    bool strike;
};

static TerminalStyle terminal_style = {
    TERMINAL_TEXT_COLOR, BACKGROUND_COLOR, false, false, false, false, false, false,
};

static int  terminal_saved_x = TERM_LEFT_X;
static int  terminal_saved_y = TERM_TOP_Y;
static int  terminal_scroll_top = 0;
static int  terminal_scroll_bottom = TERM_ROWS - 1;
static bool terminal_wrap_mode = true;
static bool terminal_origin_mode = false;
static bool terminal_insert_mode = false;

static int  terminal_parser_state = 0;
static char terminal_sequence[TERM_SEQ_MAX];
static int  terminal_sequence_len = 0;

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static UINT32 make_color(int r, int g, int b)
{
    return ((UINT32)(r & 0xff) << 24) | ((UINT32)(g & 0xff) << 16) | ((UINT32)(b & 0xff) << 8) | 0xff;
}

static int utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int terminal_col()
{
    return clamp_int((cur_x - TERM_LEFT_X) / TERM_CELL_WIDTH, 0, TERM_COLS - 1);
}

static int terminal_row()
{
    return clamp_int((cur_y - TERM_TOP_Y) / TERM_CELL_HEIGHT, 0, TERM_ROWS - 1);
}

static void terminal_set_cursor(int row, int col)
{
    row = clamp_int(row, 0, TERM_ROWS - 1);
    col = clamp_int(col, 0, TERM_COLS - 1);
    cur_x = TERM_LEFT_X + col * TERM_CELL_WIDTH;
    cur_y = TERM_TOP_Y + row * TERM_CELL_HEIGHT;
}

static UINT32 terminal_effective_fg()
{
    if (terminal_style.conceal) return terminal_style.bg;
    return terminal_style.inverse ? terminal_style.bg : terminal_style.fg;
}

static UINT32 terminal_effective_bg()
{
    return terminal_style.inverse ? terminal_style.fg : terminal_style.bg;
}

static void terminal_reset_style()
{
    terminal_style.fg = TERMINAL_TEXT_COLOR;
    terminal_style.bg = BACKGROUND_COLOR;
    terminal_style.bold = false;
    terminal_style.faint = false;
    terminal_style.underline = false;
    terminal_style.inverse = false;
    terminal_style.conceal = false;
    terminal_style.strike = false;
}

static UINT32 terminal_palette_color(int index)
{
    index = clamp_int(index, 0, 255);
    if (index < 16) return ansi_color_table[index];
    if (index >= 232)
    {
        int gray = 8 + (index - 232) * 10;
        return make_color(gray, gray, gray);
    }

    int n = index - 16;
    int level[6] = {0, 95, 135, 175, 215, 255};
    return make_color(level[n / 36], level[(n / 6) % 6], level[n % 6]);
}

static void terminal_fill_rect(int x1, int y1, int x2, int y2, UINT32 color)
{
    x1 = clamp_int(x1, 0, TML_X - 1);
    y1 = clamp_int(y1, 0, TML_Y - 1);
    x2 = clamp_int(x2, 0, TML_X - 1);
    y2 = clamp_int(y2, 0, TML_Y - 1);
    if (x2 < x1 || y2 < y1) return;
    xapi_DrawRect(handle, x1, y1, x2, y2, color, true);
}

static void terminal_save_cursor()
{
    terminal_saved_x = cur_x;
    terminal_saved_y = cur_y;
}

static void terminal_restore_cursor()
{
    cur_x = terminal_saved_x;
    cur_y = terminal_saved_y;
    terminal_set_cursor(terminal_row(), terminal_col());
}

static void terminal_scroll_region_up(int top, int bottom, int lines)
{
    top = clamp_int(top, 0, TERM_ROWS - 1);
    bottom = clamp_int(bottom, top, TERM_ROWS - 1);
    lines = clamp_int(lines, 1, bottom - top + 1);

    int y = TERM_TOP_Y + top * TERM_CELL_HEIGHT;
    int h = (bottom - top + 1) * TERM_CELL_HEIGHT;
    int move_h = h - lines * TERM_CELL_HEIGHT;
    if (move_h > 0)
    {
        xapi_ReadBuffer(handle, 0, y + lines * TERM_CELL_HEIGHT, TML_X, move_h, terminal_pixel_buffer);
        xapi_WriteBuffer(handle, 0, y, TML_X, move_h, terminal_pixel_buffer);
    }
    terminal_fill_rect(0, y + move_h, TML_X - 1, y + h - 1, terminal_effective_bg());
}

static void terminal_scroll_region_down(int top, int bottom, int lines)
{
    top = clamp_int(top, 0, TERM_ROWS - 1);
    bottom = clamp_int(bottom, top, TERM_ROWS - 1);
    lines = clamp_int(lines, 1, bottom - top + 1);

    int y = TERM_TOP_Y + top * TERM_CELL_HEIGHT;
    int h = (bottom - top + 1) * TERM_CELL_HEIGHT;
    int move_h = h - lines * TERM_CELL_HEIGHT;
    if (move_h > 0)
    {
        xapi_ReadBuffer(handle, 0, y, TML_X, move_h, terminal_pixel_buffer);
        xapi_WriteBuffer(handle, 0, y + lines * TERM_CELL_HEIGHT, TML_X, move_h, terminal_pixel_buffer);
    }
    terminal_fill_rect(0, y, TML_X - 1, y + lines * TERM_CELL_HEIGHT - 1, terminal_effective_bg());
}

static void terminal_index(bool carriage_return)
{
    int row = terminal_row();
    int col = carriage_return ? 0 : terminal_col();
    if (row == terminal_scroll_bottom) { terminal_scroll_region_up(terminal_scroll_top, terminal_scroll_bottom, 1); }
    else { row++; }
    terminal_set_cursor(row, col);
}

static void terminal_reverse_index()
{
    int row = terminal_row();
    if (row == terminal_scroll_top) { terminal_scroll_region_down(terminal_scroll_top, terminal_scroll_bottom, 1); }
    else { row--; }
    terminal_set_cursor(row, terminal_col());
}

static void terminal_erase_line(int mode)
{
    int row_y = TERM_TOP_Y + terminal_row() * TERM_CELL_HEIGHT;
    if (mode == 0) terminal_fill_rect(cur_x, row_y, TERM_RIGHT_X - 1, row_y + TERM_CELL_HEIGHT - 1, terminal_effective_bg());
    else if (mode == 1) terminal_fill_rect(TERM_LEFT_X, row_y, cur_x + TERM_CELL_WIDTH - 1, row_y + TERM_CELL_HEIGHT - 1, terminal_effective_bg());
    else terminal_fill_rect(TERM_LEFT_X, row_y, TERM_RIGHT_X - 1, row_y + TERM_CELL_HEIGHT - 1, terminal_effective_bg());
}

static void terminal_erase_display(int mode)
{
    int row = terminal_row();
    int row_y = TERM_TOP_Y + row * TERM_CELL_HEIGHT;

    if (mode == 0)
    {
        terminal_erase_line(0);
        if (row < TERM_ROWS - 1)
            terminal_fill_rect(TERM_LEFT_X, row_y + TERM_CELL_HEIGHT, TERM_RIGHT_X - 1,
                               TERM_TOP_Y + TERM_ROWS * TERM_CELL_HEIGHT - 1, terminal_effective_bg());
    }
    else if (mode == 1)
    {
        if (row > 0)
            terminal_fill_rect(TERM_LEFT_X, TERM_TOP_Y, TERM_RIGHT_X - 1, row_y - 1, terminal_effective_bg());
        terminal_erase_line(1);
    }
    else
    {
        terminal_fill_rect(0, 0, TML_X - 1, TML_Y - 1, terminal_effective_bg());
        if (mode == 3) terminal_set_cursor(0, 0);
    }
}

static void terminal_delete_chars(int count)
{
    count = clamp_int(count, 1, TERM_COLS);
    int amount = count * TERM_CELL_WIDTH;
    int row_y = TERM_TOP_Y + terminal_row() * TERM_CELL_HEIGHT;
    int copy_w = TERM_RIGHT_X - cur_x - amount;
    if (copy_w > 0)
    {
        xapi_ReadBuffer(handle, cur_x + amount, row_y, copy_w, TERM_CELL_HEIGHT, terminal_pixel_buffer);
        xapi_WriteBuffer(handle, cur_x, row_y, copy_w, TERM_CELL_HEIGHT, terminal_pixel_buffer);
    }
    terminal_fill_rect(TERM_RIGHT_X - amount, row_y, TERM_RIGHT_X - 1, row_y + TERM_CELL_HEIGHT - 1,
                       terminal_effective_bg());
}

static void terminal_insert_chars(int count)
{
    count = clamp_int(count, 1, TERM_COLS);
    int amount = count * TERM_CELL_WIDTH;
    int row_y = TERM_TOP_Y + terminal_row() * TERM_CELL_HEIGHT;
    int copy_w = TERM_RIGHT_X - cur_x - amount;
    if (copy_w > 0)
    {
        xapi_ReadBuffer(handle, cur_x, row_y, copy_w, TERM_CELL_HEIGHT, terminal_pixel_buffer);
        xapi_WriteBuffer(handle, cur_x + amount, row_y, copy_w, TERM_CELL_HEIGHT, terminal_pixel_buffer);
    }
    terminal_fill_rect(cur_x, row_y, cur_x + amount - 1, row_y + TERM_CELL_HEIGHT - 1, terminal_effective_bg());
}

static void terminal_draw_char(const char *text, int len, int width_cells)
{
    int old_col = terminal_col();
    if (terminal_col() + width_cells > TERM_COLS)
    {
        if (terminal_wrap_mode) terminal_index(true);
        else terminal_set_cursor(terminal_row(), TERM_COLS - width_cells);
        old_col = terminal_col();
    }

    if (terminal_insert_mode) terminal_insert_chars(width_cells);

    int cell_w = width_cells * TERM_CELL_WIDTH;
    int row_y = TERM_TOP_Y + terminal_row() * TERM_CELL_HEIGHT;
    terminal_fill_rect(cur_x, row_y, cur_x + cell_w - 1, row_y + TERM_CELL_HEIGHT - 1, terminal_effective_bg());

    char temp[5] = {0};
    for (int i = 0; i < len && i < 4; i++) temp[i] = text[i];
    xapi_DrawSWText(handle, cur_x, cur_y, temp, terminal_effective_fg());

    if (terminal_style.underline)
        terminal_fill_rect(cur_x, row_y + TERM_CELL_HEIGHT - 2, cur_x + cell_w - 1, row_y + TERM_CELL_HEIGHT - 2,
                           terminal_effective_fg());
    if (terminal_style.strike)
        terminal_fill_rect(cur_x, row_y + TERM_CELL_HEIGHT / 2, cur_x + cell_w - 1, row_y + TERM_CELL_HEIGHT / 2,
                           terminal_effective_fg());

    if (old_col + width_cells >= TERM_COLS)
    {
        if (terminal_wrap_mode) terminal_index(true);
        else terminal_set_cursor(terminal_row(), TERM_COLS - 1);
    }
    else
    {
        terminal_set_cursor(terminal_row(), old_col + width_cells);
    }
}

static void terminal_send_response(const char *response)
{
    int len = (int)strlen(response);
    if (len <= 0 || p_input_buffer + len >= (int)sizeof(app_input_buffer) - 1) return;
    strcpy(&app_input_buffer[p_input_buffer], response);
    p_input_buffer += len;
    input_is_enter = true;
}

static void parse_params(const char *seq, int len, bool *private_q, int *params, int *count)
{
    *private_q = false;
    *count = 0;
    int i = 0;
    if (i < len && (seq[i] == '?' || seq[i] == '>' || seq[i] == '='))
    {
        *private_q = seq[i] == '?';
        i++;
    }

    bool have = false;
    int value = 0;
    for (; i <= len && *count < TERM_PARAM_MAX; i++)
    {
        char c = i < len ? seq[i] : ';';
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            have = true;
        }
        else if (c == ';' || c == ':')
        {
            params[(*count)++] = have ? value : -1;
            have = false;
            value = 0;
        }
        else if (c >= 0x20 && c <= 0x2f)
        {
            continue;
        }
    }
}

static int param_or(int *params, int count, int index, int def_value)
{
    if (index >= count || params[index] < 0) return def_value;
    return params[index];
}

static void apply_sgr(int *params, int count)
{
    if (count == 0)
    {
        terminal_reset_style();
        return;
    }

    for (int i = 0; i < count; i++)
    {
        int p = params[i] < 0 ? 0 : params[i];
        if (p == 0) terminal_reset_style();
        else if (p == 1) terminal_style.bold = true;
        else if (p == 2) terminal_style.faint = true;
        else if (p == 4) terminal_style.underline = true;
        else if (p == 7) terminal_style.inverse = true;
        else if (p == 8) terminal_style.conceal = true;
        else if (p == 9) terminal_style.strike = true;
        else if (p == 21 || p == 22) { terminal_style.bold = false; terminal_style.faint = false; }
        else if (p == 24) terminal_style.underline = false;
        else if (p == 27) terminal_style.inverse = false;
        else if (p == 28) terminal_style.conceal = false;
        else if (p == 29) terminal_style.strike = false;
        else if (p == 39) terminal_style.fg = TERMINAL_TEXT_COLOR;
        else if (p == 49) terminal_style.bg = BACKGROUND_COLOR;
        else if (p >= 30 && p <= 37) terminal_style.fg = ansi_color_table[p - 30 + (terminal_style.bold ? 8 : 0)];
        else if (p >= 40 && p <= 47) terminal_style.bg = ansi_color_table[p - 40];
        else if (p >= 90 && p <= 97) terminal_style.fg = ansi_color_table[p - 90 + 8];
        else if (p >= 100 && p <= 107) terminal_style.bg = ansi_color_table[p - 100 + 8];
        else if ((p == 38 || p == 48) && i + 1 < count)
        {
            bool is_fg = p == 38;
            int mode = params[++i];
            if (mode == 5 && i + 1 < count)
            {
                UINT32 color = terminal_palette_color(param_or(params, count, ++i, 0));
                if (is_fg) terminal_style.fg = color;
                else terminal_style.bg = color;
            }
            else if (mode == 2 && i + 3 < count)
            {
                int r = param_or(params, count, ++i, 0);
                int g = param_or(params, count, ++i, 0);
                int b = param_or(params, count, ++i, 0);
                UINT32 color = make_color(r, g, b);
                if (is_fg) terminal_style.fg = color;
                else terminal_style.bg = color;
            }
        }
    }
}

static void terminal_reset()
{
    terminal_reset_style();
    terminal_scroll_top = 0;
    terminal_scroll_bottom = TERM_ROWS - 1;
    terminal_wrap_mode = true;
    terminal_origin_mode = false;
    terminal_insert_mode = false;
    set_terminal_cursor_enabled(true, false);
    clear_screen();
    terminal_set_cursor(0, 0);
}

static void terminal_set_mode(bool private_q, int *params, int count, bool enabled)
{
    for (int i = 0; i < count; i++)
    {
        int p = params[i];
        if (private_q)
        {
            if (p == 6)
            {
                terminal_origin_mode = enabled;
                terminal_set_cursor(enabled ? terminal_scroll_top : 0, 0);
            }
            else if (p == 7) terminal_wrap_mode = enabled;
            else if (p == 25) set_terminal_cursor_enabled(enabled, false);
            else if (p == 1048)
            {
                if (enabled) terminal_save_cursor();
                else terminal_restore_cursor();
            }
            else if (p == 1049)
            {
                if (enabled) terminal_save_cursor();
                else terminal_restore_cursor();
                terminal_erase_display(2);
                terminal_set_cursor(0, 0);
            }
        }
        else
        {
            if (p == 4) terminal_insert_mode = enabled;
        }
    }
}

static void dispatch_csi(char final_char)
{
    bool private_q = false;
    int params[TERM_PARAM_MAX];
    int count = 0;
    parse_params(terminal_sequence, terminal_sequence_len, &private_q, params, &count);

    int n = param_or(params, count, 0, 1);
    switch (final_char)
    {
    case 'A': terminal_set_cursor(terminal_row() - n, terminal_col()); break;
    case 'B': terminal_set_cursor(terminal_row() + n, terminal_col()); break;
    case 'C': terminal_set_cursor(terminal_row(), terminal_col() + n); break;
    case 'D': terminal_set_cursor(terminal_row(), terminal_col() - n); break;
    case 'E': terminal_set_cursor(terminal_row() + n, 0); break;
    case 'F': terminal_set_cursor(terminal_row() - n, 0); break;
    case 'G':
    case '`': terminal_set_cursor(terminal_row(), param_or(params, count, 0, 1) - 1); break;
    case 'H':
    case 'f': {
        int row = param_or(params, count, 0, 1) - 1;
        int col = param_or(params, count, 1, 1) - 1;
        if (terminal_origin_mode) row += terminal_scroll_top;
        terminal_set_cursor(row, col);
        break;
    }
    case 'J': terminal_erase_display(param_or(params, count, 0, 0)); break;
    case 'K': terminal_erase_line(param_or(params, count, 0, 0)); break;
    case 'L': terminal_scroll_region_down(terminal_row(), terminal_scroll_bottom, n); break;
    case 'M': terminal_scroll_region_up(terminal_row(), terminal_scroll_bottom, n); break;
    case 'P': terminal_delete_chars(n); break;
    case '@': terminal_insert_chars(n); break;
    case 'S': terminal_scroll_region_up(terminal_scroll_top, terminal_scroll_bottom, n); break;
    case 'T': terminal_scroll_region_down(terminal_scroll_top, terminal_scroll_bottom, n); break;
    case 'X':
        terminal_fill_rect(cur_x, TERM_TOP_Y + terminal_row() * TERM_CELL_HEIGHT,
                           cur_x + n * TERM_CELL_WIDTH - 1,
                           TERM_TOP_Y + (terminal_row() + 1) * TERM_CELL_HEIGHT - 1, terminal_effective_bg());
        break;
    case 'Z': terminal_set_cursor(terminal_row(), ((terminal_col() - 1) / 8) * 8); break;
    case 'a': terminal_set_cursor(terminal_row(), terminal_col() + n); break;
    case 'd': terminal_set_cursor(param_or(params, count, 0, 1) - 1, terminal_col()); break;
    case 'e': terminal_set_cursor(terminal_row() + n, terminal_col()); break;
    case 'm': apply_sgr(params, count); break;
    case 'n':
        if (param_or(params, count, 0, 0) == 5) terminal_send_response("\x1b[0n");
        else if (param_or(params, count, 0, 0) == 6)
        {
            char response[32];
            int row = terminal_row() + 1;
            int col = terminal_col() + 1;
            snprintf(response, sizeof(response), "\x1b[%d;%dR", row, col);
            terminal_send_response(response);
        }
        break;
    case 'r': {
        int top = param_or(params, count, 0, 1) - 1;
        int bottom = param_or(params, count, 1, TERM_ROWS) - 1;
        if (top < 0) top = 0;
        if (bottom >= TERM_ROWS) bottom = TERM_ROWS - 1;
        if (top < bottom)
        {
            terminal_scroll_top = top;
            terminal_scroll_bottom = bottom;
            terminal_set_cursor(terminal_origin_mode ? top : 0, 0);
        }
        break;
    }
    case 's': terminal_save_cursor(); break;
    case 'u': terminal_restore_cursor(); break;
    case 'h': terminal_set_mode(private_q, params, count, true); break;
    case 'l': terminal_set_mode(private_q, params, count, false); break;
    default: break;
    }
}

static void dispatch_osc()
{
    int sep = -1;
    for (int i = 0; i < terminal_sequence_len; i++)
    {
        if (terminal_sequence[i] == ';')
        {
            sep = i;
            break;
        }
    }
    if (sep <= 0 || sep >= terminal_sequence_len - 1) return;

    int code = 0;
    for (int i = 0; i < sep; i++)
    {
        if (terminal_sequence[i] < '0' || terminal_sequence[i] > '9') return;
        code = code * 10 + (terminal_sequence[i] - '0');
    }

    if (code == 0 || code == 2)
    {
        terminal_sequence[terminal_sequence_len] = '\0';
        xapi_SetWindowTitle(handle, &terminal_sequence[sep + 1]);
    }
}

static void terminal_begin_sequence(int state)
{
    terminal_parser_state = state;
    terminal_sequence_len = 0;
}

static void terminal_append_sequence(unsigned char ch)
{
    if (terminal_sequence_len < TERM_SEQ_MAX - 1)
        terminal_sequence[terminal_sequence_len++] = (char)ch;
}

static void process_escape_byte(unsigned char ch)
{
    switch (ch)
    {
    case '[': terminal_begin_sequence(2); break;
    case ']': terminal_begin_sequence(3); break;
    case 'P':
    case '^':
    case '_':
    case 'X': terminal_begin_sequence(5); break;
    case '7': terminal_save_cursor(); terminal_parser_state = 0; break;
    case '8': terminal_restore_cursor(); terminal_parser_state = 0; break;
    case 'c': terminal_reset(); terminal_parser_state = 0; break;
    case 'D': terminal_index(false); terminal_parser_state = 0; break;
    case 'E': terminal_index(true); terminal_parser_state = 0; break;
    case 'M': terminal_reverse_index(); terminal_parser_state = 0; break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
    case '#': terminal_parser_state = 6; break;
    default: terminal_parser_state = 0; break;
    }
}

static void process_control(unsigned char ch)
{
    if (ch == '\a') return;
    if (ch == '\b')
    {
        if (terminal_col() > 0) terminal_set_cursor(terminal_row(), terminal_col() - 1);
        return;
    }
    if (ch == '\t')
    {
        terminal_set_cursor(terminal_row(), ((terminal_col() / 8) + 1) * 8);
        return;
    }
    if (ch == '\n' || ch == '\v' || ch == '\f')
    {
        terminal_index(true);
        return;
    }
    if (ch == '\r')
    {
        terminal_set_cursor(terminal_row(), 0);
        return;
    }
}

static void terminal_process_byte(unsigned char ch)
{
    if (terminal_parser_state == 1)
    {
        process_escape_byte(ch);
        return;
    }
    if (terminal_parser_state == 2)
    {
        if (ch >= 0x40 && ch <= 0x7e)
        {
            dispatch_csi((char)ch);
            terminal_parser_state = 0;
        }
        else terminal_append_sequence(ch);
        return;
    }
    if (terminal_parser_state == 3)
    {
        if (ch == '\a')
        {
            dispatch_osc();
            terminal_parser_state = 0;
        }
        else if (ch == '\x1b') terminal_parser_state = 4;
        else terminal_append_sequence(ch);
        return;
    }
    if (terminal_parser_state == 4)
    {
        if (ch == '\\')
        {
            dispatch_osc();
            terminal_parser_state = 0;
        }
        else
        {
            terminal_append_sequence('\x1b');
            terminal_append_sequence(ch);
            terminal_parser_state = 3;
        }
        return;
    }
    if (terminal_parser_state == 5)
    {
        if (ch == '\x1b') terminal_parser_state = 7;
        return;
    }
    if (terminal_parser_state == 6)
    {
        terminal_parser_state = 0;
        return;
    }
    if (terminal_parser_state == 7)
    {
        terminal_parser_state = (ch == '\\') ? 0 : 5;
        return;
    }

    if (ch == '\x1b')
    {
        terminal_parser_state = 1;
        return;
    }
    if (ch < 0x20 || ch == 0x7f)
    {
        process_control(ch);
        return;
    }
}

void ptt_console_sp(char *str)
{
    hide_terminal_cursor(false);
    char *p = str;
    while (*p != '\0')
    {
        if (terminal_parser_state != 0 || *p == '\x1b' || (unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
        {
            terminal_process_byte((unsigned char)*p++);
            continue;
        }

        int char_len = utf8_char_len((unsigned char)*p);
        int copied = 0;
        for (; copied < char_len && p[copied] != '\0'; copied++) {}
        terminal_draw_char(p, copied, char_len == 1 ? 1 : 2);
        p += copied;
    }
}
void tam_main_loop()
{
    cur_y += 2;
    reset_terminal_cursor_blink();
    show_terminal_cursor(true);
    while (true)
    {
        // 获取输入
        xapi_Sleep(16);
        __asm__ volatile("pause\n\t");
        if (p_chbuffer) { tam_flush_buffer(); }
        update_terminal_cursor_blink(16);

        // 检查输出缓冲区
        char output_stream[1024];
        memset(output_stream, 0, sizeof(output_stream));
        read_xttp_buffer(output_stream);
        if (output_stream[0] != '\0') 
        {
            ptt_console_sp(output_stream);
            reset_terminal_cursor_blink();
            show_terminal_cursor(false);
            memset(output_stream, 0, (strlen(output_stream) + 1) * sizeof(uint8_t));
            xapi_RefreshWindow(handle);
            terminal_app_mark_finish_output();
        }

        // 检查输入缓冲区
        if (check_read_xttp_buffer() && input_is_enter)
        {
            newline();
            input_is_enter = false;
            strcat(app_input_buffer, "\0");
            write_xttp_buffer(app_input_buffer);
            app_input_buffer[0] = '\0';
            p_input_buffer      = 0;
            reset_terminal_cursor_blink();
            show_terminal_cursor(true);
        }
    }
}
