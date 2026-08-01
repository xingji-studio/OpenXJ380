#include "../xapi/include/krlibc.h"
#include "../xapi/include/x3api.h"
#include "convert.h"
#include "gh_sign.h"
#include "st_proto.h"

char* user_type_convert_table[5] = {
    "root",
    "system",
    "admin",
    "visitor",
    "~"
};

// 全局变量定义
int  cur_x  = 0;
int  cur_y  = 0;
int *ab_x   = &cur_x;
int *ab_y   = &cur_y;

char char_buffer[64];
int  p_chbuffer = 0;

char input_buf[1024];
int  p_inbuf = 0;

char current_path[512];

bool front_line_is_arrow = false;
bool command_ck_lock     = false;
bool putchar_lock        = false;
bool paint_cursor_lock   = false;

bool server_mode = false;
bool shell_prompt_pending = false;
int shell_running_external_pid = -1;
int shell_language = XJ380_LANGUAGE_ZH_CN;

HDLE handle;

int blkc = 0, cf = 0;

char *shell_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(shell_language, zh_cn, en_us);
}

void shell_init_language()
{
    shell_language = xj380_read_language();
}

void shell_MessagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    if (shell_prompt_pending)
    {
        switch (Type)
        {
        case MSG_CHAR: terminal_child_putchar((char)lData); break;
        case MSG_SPCHAR:
            if (lData == '\n') { terminal_child_submit_line(); }
            if (lData == '\b') { terminal_child_backspace(); }
            break;
        }
        return;
    }

    switch (Type)
    {
    case MSG_CHAR: putchar_at_cur((char *)&lData); break;
    case MSG_SPCHAR:
        if (lData == '\n') { check_command(); }
        if (lData == '\b') { backspace(); }
        break;
    }
    return;
}

static void poll_external_command_prompt()
{
    if (!shell_prompt_pending || shell_running_external_pid <= 0) return;

    int status = 0;
    int pid = waitpid(shell_running_external_pid, &status, WNOHANG);
    if (pid == 0) return;

    shell_running_external_pid = -1;
    shell_prompt_pending = false;
    if (cur_x != 8) { newline(); }
    paint_arrow_sign(2, cur_y + 4);
}

static int shell_main_impl(int argc, char *argv[], char *envp[])
{
    shell_init_language();

    XWINDOW Winfo;
    Winfo.title  = shell_tr("终端", "Terminal");
    Winfo.width  = TML_X;
    Winfo.height = TML_Y;
    Winfo.sets   = XWIN_NORMAL;
    xapi_CreateWindow(&handle, &Winfo);
    xapi_SetIcon(handle, "/system/icon/terminal.png");
    SetMsgPrcor(handle, shell_MessagePrcor);

    mark_terminal();

    // 初始绘制
    *ab_x = 8;
    *ab_y = 4;
    xapi_DrawRect(handle, 0, 0, TML_X - 1, TML_Y - 1, BACKGROUND_COLOR, true);

    if (argv != NULL && argv[0] != NULL)
    {
        if (strcmp(argv[0], "-xj380sys-terminal-app-mode") == 0)
        {
            // 这代表是给别的命令行程序用的
            server_mode = true;
            xapi_RefreshWindow(handle);
            tam_main_loop();
            return 0;
        }
    }

    if (getcwd(current_path, sizeof(current_path)) == NULL) { strcpy(current_path, "/"); }
    print_to_console_zh("XINGJI SpaceTerminal for XJ380");
    print_to_console_zh(shell_tr("版权所有(C) XINGJI Interactive Software 2017-2026。保留所有权利。",
                                 "Copyright (C) XINGJI Interactive Software 2017-2026. All rights reserved."));
    paint_arrow_sign(2, 40);
    reset_terminal_cursor_blink();
    show_terminal_cursor(true);
    xapi_RefreshWindow(handle);
    while (1)
    {
        // 主循环中检查并刷新缓冲区
        if (p_chbuffer) { flush_buffer(); }

        char output_stream[1024];
        memset(output_stream, 0, sizeof(output_stream));
        read_xttp_buffer(output_stream);
        if (output_stream[0] != '\0')
        {
            ptt_console_sp(output_stream);
            reset_terminal_cursor_blink();
            show_terminal_cursor(false);
            xapi_RefreshWindow(handle);
            terminal_app_mark_finish_output();
        }

        update_terminal_cursor_blink(16);
        xapi_Sleep(16);
        __asm__ __volatile__("pause");
    }
}

extern "C" int shell_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int shell_main_cpp(int argc, char *argv[], char *envp[])
{
    return shell_main_impl(argc, argv, envp);
}
