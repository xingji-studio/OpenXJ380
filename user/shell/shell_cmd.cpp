#include "st_proto.h"
#include "gh_sign.h"
#include <stdint.h>

uint64_t rp = 0;

extern bool input_is_enter; // shell_tam.cpp
extern bool server_mode; // shell_main.cpp

static int build_external_argv(char *exe_path, char *args, char *argv[], int max_argc)
{
    int argc = 0;
    if (max_argc <= 1) return 0;

    argv[argc++] = exe_path;
    while (args != NULL && *args != '\0' && argc < max_argc - 1)
    {
        while (*args == ' ')
            args++;
        if (*args == '\0') break;

        char quote = 0;
        if (*args == '\'' || *args == '"')
        {
            quote = *args;
            args++;
        }

        argv[argc++] = args;
        char *out = args;
        while (*args != '\0')
        {
            if (quote)
            {
                if (*args == quote)
                {
                    args++;
                    break;
                }
            }
            else if (*args == ' ')
            {
                break;
            }

            if (*args == '\\' && args[1] != '\0') args++;
            *out++ = *args++;
        }
        bool has_more = *args != '\0';
        *out = '\0';
        if (!has_more) break;
        args++;
    }
    argv[argc] = NULL;
    return argc;
}

static bool append_to_buffer(char *out, size_t out_size, const char *suffix)
{
    size_t used = strlen(out);
    size_t add  = strlen(suffix);
    if (used + add + 1 > out_size) return false;
    strcat(out, suffix);
    return true;
}

static bool resolve_external_command_path(char *cmd, char *out, size_t out_size)
{
    if (cmd == NULL || *cmd == '\0' || out == NULL || out_size == 0) return false;

    out[0] = '\0';
    if (cmd[0] == '/')
    {
        strncpy(out, cmd, out_size - 1);
        out[out_size - 1] = '\0';
        normalize_path(out);
        return true;
    }

    if (strchr(cmd, '/') != NULL)
    {
        strncpy(out, current_path, out_size - 1);
        out[out_size - 1] = '\0';
        if (out[strlen(out) - 1] != '/' && !append_to_buffer(out, out_size, "/")) return false;
        if (!append_to_buffer(out, out_size, cmd)) return false;
        normalize_path(out);
        return true;
    }

    if (check_dir_com(cmd) != 1) return false;
    strncpy(out, current_path, out_size - 1);
    out[out_size - 1] = '\0';
    if (out[strlen(out) - 1] != '/' && !append_to_buffer(out, out_size, "/")) return false;
    if (!append_to_buffer(out, out_size, cmd)) return false;
    normalize_path(out);
    return true;
}

static int run_external_command(char *cmd, char *args)
{
    char runfile_path[512];
    if (!resolve_external_command_path(cmd, runfile_path, sizeof(runfile_path))) return 0;

    char *argv[32];
    build_external_argv(runfile_path, args, argv, 32);
    // xapi_OutputSerial((char *)"[busybox-debug] shell run_external path=");
    // xapi_OutputSerial(runfile_path);
    // xapi_OutputSerial((char *)"\n");
    // for (int i = 0; argv[i] != NULL; i++)
    // {
    //     xapi_OutputSerial((char *)"[busybox-debug] shell argv[");
    //     char idx[16];
    //     snprintf(idx, sizeof(idx), "%d", i);
    //     xapi_OutputSerial(idx);
    //     xapi_OutputSerial((char *)"]=");
    //     xapi_OutputSerial(argv[i]);
    //     xapi_OutputSerial((char *)"\n");
    // }

    int64_t pid = (int64_t)xapi_RunArgs(runfile_path, argv);
    if (pid < 0)
    {
        print_to_console_zh(shell_tr("运行失败", "Run failed"));
        return -1;
    }
    return (int)pid;
}

void check_command()
{
    if (server_mode)
    {
        input_is_enter = true;
        return;
    }

    command_ck_lock = true;

    input_buf[p_inbuf] = '\0';
    newline();

    char *cmd  = input_buf;
    char *args = NULL;
    bool should_paint_prompt = true;

    // 分割命令和参数
    for (int i = 0; i < p_inbuf; i++)
    {
        if (input_buf[i] == ' ')
        {
            input_buf[i] = '\0';
            args         = input_buf + i + 1;
            while (*args == ' ')
                args++;
            break;
        }
    }

    bool paint_prompt = true;

    if (strncmp("version", cmd, 7) == 0)
    {
        print_to_console_zh(shell_tr("XINGJI SpaceTerminal for XJ380 [正式版]",
                                     "XINGJI SpaceTerminal for XJ380 [Release]"));
        print_to_console_zh(shell_tr("版本 1.0.0 - 2026/10/4   平台：仅适用于 XJ380",
                                     "Version 1.0.0 - 2026/10/4   Platform: XJ380 only"));
        print_to_console_zh(shell_tr("版权所有(C) XINGJI Interactive Software 2017-2025。保留所有权利。",
                                     "Copyright (C) XINGJI Interactive Software 2017-2025. All rights reserved."));
    }
    else if (strncmp("dir", cmd, 3) == 0 || strncmp("ls", cmd, 2) == 0) { print_dir(); }
    else if (strncmp("fork", cmd, 4) == 0)
    {
        uint64_t pid = xapi_Fork();
        xapi_Execve("/apps/system/shell.elf", NULL, NULL);
    }
    else if (strncmp("mkdir", cmd, 5) == 0)
    {
        if (args == NULL || *args == '\0')
        {
            print_to_console_zh(shell_tr("mkdir：缺少操作数", "mkdir: missing operand"));
            print_to_console_zh(shell_tr("用法：mkdir [-p] 目录名", "Usage: mkdir [-p] directory"));
        }
        else
        {
            create_directory(args);
        }
    }
    else if (strncmp("cd", cmd, 2) == 0)
    {
        if (args == NULL || *args == '\0') { change_directory((char *)"/"); }
        else
        {
            change_directory(args);
        }
    }
    else if (strncmp("cat", cmd, 3) == 0)
    {
        if (args == NULL || *args == '\0')
        {
            print_to_console_zh(shell_tr("cat：缺少操作数", "cat: missing operand"));
            print_to_console_zh(shell_tr("用法：cat 文件名", "Usage: cat file"));
        }
        else
        {
            print_file_content(args);
        }
    }
    else if (strncmp("touch", cmd, 5) == 0)
    {
        if (args == NULL || *args == '\0')
        {
            print_to_console_zh(shell_tr("touch：缺少操作数", "touch: missing operand"));
            print_to_console_zh(shell_tr("用法：touch 文件名", "Usage: touch file"));
        }
        else
        {
            create_file(args);
        }
    }
    else if (strncmp("rm", cmd, 2) == 0)
    {
        if (args == NULL || *args == '\0')
        {
            print_to_console_zh(shell_tr("rm：缺少操作数", "rm: missing operand"));
            print_to_console_zh(shell_tr("用法：rm 文件名", "Usage: rm file"));
        }
        else
        {
            remove_file_or_directory(args);
        }
    }
    else if (strncmp("cp", cmd, 2) == 0)
    {
        char *src = args;
        char *dst = NULL;

        if (src)
        {
            while (*src && *src != ' ')
                src++;
            if (*src)
            {
                *src = '\0';
                src  = args;
                dst  = src + strlen(src) + 1;
                while (*dst == ' ')
                    dst++;
            }
        }

        if (src == NULL || dst == NULL || *dst == '\0')
        {
            print_to_console_zh(shell_tr("cp：缺少操作数", "cp: missing operand"));
            print_to_console_zh(shell_tr("用法：cp 源 目标", "Usage: cp source target"));
        }
        else
        {
            copy_file(src, dst);
        }
    }
    else if (strncmp("mv", cmd, 2) == 0)
    {
        //会PF
        // char *src = args;
        // char *dst = NULL;

        // if (src)
        // {
        //     while (*src && *src != ' ')
        //         src++;
        //     if (*src)
        //     {
        //         *src = '\0';
        //         src  = args;
        //         dst  = src + strlen(src) + 1;
        //         while (*dst == ' ')
        //             dst++;
        //     }
        // }

        // if (src == NULL || dst == NULL || *dst == '\0')
        // {
        //     print_to_console("mv: missing operand");
        //     print_to_console_zh("用法：mv 源 目标");
        // }
        // else
        // {
        //     move_file(src, dst);
        // }
    }
    else if (strncmp("pwd", cmd, 3) == 0) { print_working_directory(); }
    else if (strncmp("clear", cmd, 5) == 0) { clear_screen(); }
    else if (strncmp("taskmgr", cmd, 7) == 0)
    {
        should_paint_prompt = false;
        int status = 0;
        uint64_t pid = fork();
        if ((int64_t)pid < 0)
        {
            should_paint_prompt = true;
            print_to_console_zh(shell_tr("taskmgr：启动进程失败", "taskmgr: failed to start process"));
        }
        else if (pid == 0)
        {
            char *taskmgr_argv[] = {(char *)"/apps/system/taskmgr.elf", NULL};
            xapi_Execve("/apps/system/taskmgr.elf", taskmgr_argv, NULL);
            exit(127);
        }
        else
        {
            waitpid((int)pid, &status, 0);
        }
    }
    else if (strcmp(cmd, "nut") == 0)
    {
        char *nut_argv[16] = {0};
        int   nut_argc = 1;

        nut_argv[0] = (char *)"/apps/builtin/nut.elf";

        if (args != NULL && *args != '\0')
        {
            char *p = args;
            while (*p != '\0' && nut_argc < 15)
            {
                while (*p == ' ')
                {
                    p++;
                }

                if (*p == '\0')
                {
                    break;
                }

                nut_argv[nut_argc++] = p;

                while (*p != '\0' && *p != ' ')
                {
                    p++;
                }

                if (*p == '\0')
                {
                    break;
                }

                *p = '\0';
                p++;
            }
        }

        nut_argv[nut_argc] = NULL;

        should_paint_prompt = false;
        int status = 0;
        uint64_t pid = fork();
        if ((int64_t)pid < 0)
        {
            should_paint_prompt = true;
            print_to_console_zh(shell_tr("nut：启动进程失败", "nut: failed to start process"));
        }
        else if (pid == 0)
        {
            uint64_t exec_ret = xapi_Execve("/apps/builtin/nut.elf", nut_argv, NULL);
            char     nut_log[128];
            snprintf(nut_log,
                     sizeof(nut_log),
                     shell_tr("nut：execve 失败 ret=%d", "nut: execve failed ret=%d"),
                     (int)((int64_t)exec_ret));
            xapi_OutputSerial(nut_log);
            print_to_console(nut_log);
            exit(127);
        }
        else
        {
            waitpid((int)pid, &status, 0);
            should_paint_prompt = true;
        }
    }
    else if (strncmp("help", cmd, 4) == 0)
    {
        print_to_console_zh(shell_tr("可用命令：", "Available commands:"));
        print_to_console_zh(shell_tr("  ls, dir     - 列出目录内容", "  ls, dir     - List directory contents"));
        print_to_console_zh(shell_tr("  cd          - 切换目录", "  cd          - Change directory"));
        print_to_console_zh(shell_tr("  pwd         - 显示当前目录", "  pwd         - Show current directory"));
        print_to_console_zh(shell_tr("  mkdir       - 创建目录", "  mkdir       - Create a directory"));
        print_to_console_zh(shell_tr("  touch       - 创建空文件", "  touch       - Create an empty file"));
        print_to_console_zh(shell_tr("  cat         - 显示文件内容", "  cat         - Print file contents"));
        print_to_console_zh(shell_tr("  rm          - 删除文件或目录", "  rm          - Remove a file or directory"));
        print_to_console_zh(shell_tr("  cp          - 复制文件", "  cp          - Copy a file"));
        print_to_console_zh(shell_tr("  mv          - 移动/重命名文件", "  mv          - Move or rename a file"));
        print_to_console_zh(shell_tr("  clear       - 清屏", "  clear       - Clear the screen"));
        print_to_console_zh(shell_tr("  version     - 显示版本信息", "  version     - Show version information"));
        print_to_console_zh(shell_tr("  help        - 显示此帮助", "  help        - Show this help"));
        print_to_console_zh(shell_tr("  edit        - 编辑文件", "  edit        - Edit a file"));
        print_to_console_zh(shell_tr("  taskmgr     - 打开任务管理器", "  taskmgr     - Open Task Manager"));
        print_to_console_zh(shell_tr("  <程序> [参数...] - 带参数运行可执行文件",
                                     "  <program> [args...] - Run an executable with arguments"));
    }
    else if (strncmp("../", cmd, 3) == 0) { change_directory(cmd); }
    else if (strncmp("edit", cmd, 4) == 0)
    {
        // xapi_Execve("/system/shell.elf",NULL,NULL);
    }
    else if (strncmp("info", cmd, 4) == 0)
    {
        char version_info[100];
        xapi_GetSystemVersion(version_info);
        print_to_console(info_logo[0]);
        print_to_console(info_logo[1]);
        print_to_console(info_logo[2]);
        print_to_console(info_logo[3]);
        print_to_console(info_logo[4]);
        print_to_console(info_logo[5]);
        print_to_console(version_info);
    }else if(strncmp("RP++", cmd, 4)==0)
    {
        print_to_console_zh(shell_tr("你的 RP：", "Your RP:"));
        rp++;
        char buf[1024];
        uint64_to_string(rp,buf,sizeof(buf));
        print_to_console(buf);
        print_to_console_zh(shell_tr(" 祝你2026 CSP J/S 一等奖!\n", " Good luck with 2026 CSP J/S!\n"));
    }
    else
    {
        if (check_dir_com(input_buf) == 1)
        {
            char runfile_path[256];
            memset(runfile_path, 0, sizeof(runfile_path));
            strcat(runfile_path, current_path);
            strcat(runfile_path, "/");
            strcat(runfile_path, input_buf);
            normalize_path(runfile_path);
            should_paint_prompt = false;
            int status = 0;
            uint64_t pid = fork();
            if ((int64_t)pid < 0)
            {
                should_paint_prompt = true;
                print_to_console_zh(shell_tr("run：启动进程失败", "run: failed to start process"));
            }
            else if (pid == 0)
            {
                char *run_argv[] = {runfile_path, NULL};
                xapi_Execve(runfile_path, run_argv, NULL);
                exit(127);
            }
            else
            {
                waitpid((int)pid, &status, 0);
            }
        }
        else if (check_dir_com(cmd) == 2)
        {
            strcat(current_path, "/");
            strcat(current_path, cmd);
        }
        else if (p_inbuf > 0)
        {
            print_to_console_zh(shell_tr("不存在的命令或路径。请检查您的输入是否有误并再试一次。",
                                         "No such command or path. Check your input and try again."));
            print_to_console_zh(shell_tr("错误命令。请检查后重试。", "Invalid command. Check it and try again."));
        }
    }

    p_inbuf = 0;
    if (should_paint_prompt)
    {
        paint_arrow_sign(2, cur_y + 4);
    }
    command_ck_lock = false;
}
