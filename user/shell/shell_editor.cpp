#include "st_proto.h"

// 文本编辑器全局变量
char editor_buffer[8192];
int  editor_cursor  = 0;
bool editor_running = false;

char *editor_read_file(char *filename)
{
    char full_path[512];

    if (filename[0] == '/') { strcpy(full_path, filename); }
    else
    {
        strcpy(full_path, current_path);
        if (full_path[strlen(full_path) - 1] != '/') strcat(full_path, "/");
        strcat(full_path, filename);
    }

    int ret = xapi_ReadFile(full_path, editor_buffer, sizeof(editor_buffer) - 1, 0);
    if (ret < 0)
    {
        editor_buffer[0] = '\0';
    }
    else
    {
        editor_buffer[ret] = '\0';
    }

    editor_cursor = 0;
    return editor_buffer;
}

void editor_save_file(char *filename, char *content)
{
    char full_path[512];

    if (filename[0] == '/') { strcpy(full_path, filename); }
    else
    {
        strcpy(full_path, current_path);
        if (full_path[strlen(full_path) - 1] != '/') strcat(full_path, "/");
        strcat(full_path, filename);
    }

    int ret = xapi_WriteFile(full_path, content, strlen(content), 0);
    if (ret < 0)
    {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), shell_tr("编辑器: 无法保存文件 '%s'", "Editor: cannot save file '%s'"), filename);
        print_to_console(error_msg);
    }
    else
    {
        char success_msg[256];
        snprintf(success_msg,
                 sizeof(success_msg),
                 shell_tr("编辑器: 文件 '%s' 已保存 (%d 字节)", "Editor: saved '%s' (%d bytes)"),
                 filename,
                 (int)strlen(content));
        print_to_console(success_msg);
    }
}

void editor_display_help()
{
    print_to_console(shell_tr("=== 文本编辑器命令 ===", "=== Text Editor Commands ==="));
    print_to_console(shell_tr("i [文本]    - 在当前位置插入文本", "i [text]    - Insert text at the current position"));
    print_to_console(shell_tr("d [行号]    - 删除指定行 (不指定则删除当前行)",
                              "d [line]    - Delete a line (current line if omitted)"));
    print_to_console(shell_tr("l           - 显示所有行", "l           - Show all lines"));
    print_to_console(shell_tr("l [起始]-[结束] - 显示指定范围的行",
                              "l [start]-[end] - Show a range of lines"));
    print_to_console(shell_tr("s /旧文本/新文本 - 替换文本", "s /old/new/ - Replace text"));
    print_to_console(shell_tr("g [行号]    - 跳转到指定行", "g [line]    - Go to a line"));
    print_to_console(shell_tr("c           - 清空缓冲区", "c           - Clear the buffer"));
    print_to_console(shell_tr("w           - 保存文件", "w           - Save the file"));
    print_to_console(shell_tr("q           - 退出编辑器", "q           - Quit the editor"));
    print_to_console(shell_tr("h           - 显示此帮助", "h           - Show this help"));
    print_to_console("=====================");
}

char *editor_get_line(int line_num, char *buffer, int buffer_size)
{
    static char line_buf[256];
    char       *start        = buffer;
    int         current_line = 1;

    while (*start && current_line < line_num)
    {
        if (*start == '\n') { current_line++; }
        start++;
    }

    if (current_line < line_num) { return NULL; }

    int i = 0;
    while (*start && *start != '\n' && i < buffer_size - 1)
    {
        line_buf[i++] = *start++;
    }
    line_buf[i] = '\0';

    return line_buf;
}

int editor_count_lines(char *buffer)
{
    int   lines = 1;
    char *ptr   = buffer;

    while (*ptr)
    {
        if (*ptr == '\n') { lines++; }
        ptr++;
    }

    return lines;
}

static inline void *editor_memmove(void *dest, const void *src, size_t n)
{
    uint8_t       *pdest = (uint8_t *)dest;
    const uint8_t *psrc  = (const uint8_t *)src;

    if (src > dest)
    {
        for (size_t i = 0; i < n; i++) { pdest[i] = psrc[i]; }
    }
    else if (src < dest)
    {
        for (size_t i = n; i > 0; i--) { pdest[i - 1] = psrc[i - 1]; }
    }

    return dest;
}

void text_editor(char *filename)
{
    if (filename == NULL || *filename == '\0')
    {
        print_to_console_zh(shell_tr("编辑器: 缺少文件名", "Editor: missing file name"));
        print_to_console_zh(shell_tr("用法: edit 文件名", "Usage: edit file"));
        return;
    }

    char *content     = editor_read_file(filename);
    int   total_lines = editor_count_lines(content);

    char welcome_msg[256];
    snprintf(welcome_msg,
             sizeof(welcome_msg),
             shell_tr("文本编辑器 - 编辑: %s (%d 行, %d 字节)", "Text Editor - Editing: %s (%d lines, %d bytes)"),
             filename,
             total_lines,
             (int)strlen(content));
    print_to_console_zh(welcome_msg);
    print_to_console_zh(shell_tr("输入 'h' 查看帮助命令", "Type 'h' to show help commands"));

    editor_running = true;
    char edit_cmd[256];
    char edit_args[256];

    while (editor_running)
    {
        char prompt[32];
        int  current_line = editor_count_lines(editor_buffer) - editor_count_lines(editor_buffer + editor_cursor) + 1;
        snprintf(prompt, sizeof(prompt), "EDIT[%d]:", current_line);
        xapi_DrawSWText(handle, 8, cur_y, prompt, TERMINAL_TEXT_COLOR);

        p_inbuf = 0;
        while (p_inbuf == 0) {
            __asm__ __volatile__("pause");
        }
        input_buf[p_inbuf] = '\0';
        strcpy(edit_cmd, input_buf);

        char  command = edit_cmd[0];
        char *args    = edit_cmd + 1;
        while (*args == ' ')
            args++;

        switch (command)
        {
        case 'i':
            if (strlen(args) > 0)
            {
                int len = strlen(args);
                if (strlen(editor_buffer) + len < sizeof(editor_buffer) - 1)
                {
                    editor_memmove(editor_buffer + editor_cursor + len, editor_buffer + editor_cursor,
                                   strlen(editor_buffer + editor_cursor) + 1);
                    memcpy(editor_buffer + editor_cursor, args, len);
                    editor_cursor += len;

                    char msg[64];
                    snprintf(msg, sizeof(msg), shell_tr("已插入 %d 字符", "Inserted %d characters"), len);
                    print_to_console_zh(msg);
                }
                else
                {
                    print_to_console_zh(shell_tr("错误: 缓冲区已满", "Error: buffer is full"));
                }
            }
            break;

        case 'l':
            if (strlen(args) == 0)
            {
                char *line_start = editor_buffer;
                int   line_num   = 1;

                while (*line_start)
                {
                    char  line_display[300];
                    char *line_end = strchr(line_start, '\n');
                    int   line_len = line_end ? (line_end - line_start) : strlen(line_start);

                    if (line_len > 250) line_len = 250;
                    strncpy(line_display, line_start, line_len);
                    line_display[line_len] = '\0';

                    char line_num_str[16];
                    snprintf(line_num_str, sizeof(line_num_str), "%3d: ", line_num);

                    char full_line[320];
                    strcpy(full_line, line_num_str);
                    strcat(full_line, line_display);
                    print_to_console(full_line);

                    if (line_end) { line_start = line_end + 1; }
                    else { break; }
                    line_num++;
                }
            }
            break;

        case 'd':
            {
                char *line_start = editor_buffer;
                char *line_end   = strchr(editor_buffer, '\n');
                if (line_end)
                {
                    int line_len = line_end - editor_buffer;
                    editor_memmove(editor_buffer, line_end + 1, strlen(line_end + 1) + 1);
                    editor_cursor = 0;
                    print_to_console_zh(shell_tr("已删除当前行", "Deleted current line"));
                }
                else
                {
                    editor_buffer[0] = '\0';
                    editor_cursor    = 0;
                    print_to_console_zh(shell_tr("已删除当前行", "Deleted current line"));
                }
            }
            break;

        case 's':
            {
                char *old_text = args;
                char *new_text = NULL;

                if (*old_text == '/')
                {
                    old_text++;
                    new_text = strchr(old_text, '/');
                    if (new_text)
                    {
                        *new_text = '\0';
                        new_text++;
                        char *end = strchr(new_text, '/');
                        if (end) *end = '\0';
                    }
                }

                if (old_text && new_text)
                {
                    char *pos = strstr(editor_buffer, old_text);
                    if (pos)
                    {
                        int old_len = strlen(old_text);
                        int new_len = strlen(new_text);

                        if (strlen(editor_buffer) + new_len - old_len < sizeof(editor_buffer) - 1)
                        {
                            editor_memmove(pos + new_len, pos + old_len, strlen(pos + old_len) + 1);
                            memcpy(pos, new_text, new_len);
                            print_to_console_zh(shell_tr("已替换文本", "Replaced text"));
                        }
                        else
                        {
                            print_to_console_zh(shell_tr("错误: 替换后缓冲区会溢出", "Error: replacement would overflow the buffer"));
                        }
                    }
                    else
                    {
                        print_to_console_zh(shell_tr("未找到匹配的文本", "No matching text found"));
                    }
                }
                else
                {
                    print_to_console_zh(shell_tr("用法: s /旧文本/新文本/", "Usage: s /old/new/"));
                }
            }
            break;

        case 'c':
            editor_buffer[0] = '\0';
            editor_cursor    = 0;
            print_to_console_zh(shell_tr("缓冲区已清空", "Buffer cleared"));
            break;

        case 'w':
            editor_save_file(filename, editor_buffer);
            break;

        case 'q':
            editor_running = false;
            print_to_console_zh(shell_tr("退出编辑器", "Quit editor"));
            break;

        case 'h':
            editor_display_help();
            break;

        default:
            print_to_console_zh(shell_tr("未知命令。输入 'h' 查看帮助。", "Unknown command. Type 'h' for help."));
            break;
        }
    }
}
