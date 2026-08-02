/*
 *
 *      X3TP (aka XTTTP)
 *      XJ380 Terminal Text Transmit Protocol
 *      XJ380 终端文本传输协议
 * 
 *      XINGJI Interactive Software (C) 2017-2026 All rights reserved.
 * 
 *      ===介绍===
 *      为图形化操作系统终端与应用程序间的文本传输提供更加简易与高效的解决方案。
 *      
 *      ===基本原理===
 *      通过 TCB 保存输入输出信息，根据其父进程搜索目标终端（需保证终端的父进程为NULL或kernel group）。
 * 
 */

#pragma once

typedef struct 
{
    bool  is_shell;
    bool  input_lock;       // false = waiting input
    bool  output_lock;      // false = waiting flush
    bool  wait_for_getch;   // true  = waiting getch
    bool  wait_for_input;   // true  = waiting input (for console)
    char  char_for_getch;
    char input[1024];
    char output[1024];
} xtttp_dtt;

int check_terminal_init_status();
void mark_process_is_terminal();
int check_input_waiting_status();
int read_terminal_app_output_buffer(char *str);
int write_terminal_app_output_buffer(const char *str);
void terminal_finish_app_output();
