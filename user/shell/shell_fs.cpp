#include "st_proto.h"
#include <xposix/errno.h>

void revert_path(char *path)
{
    char *s = path;
    while (*s)
        s++;

    while (*s != '/')
        s--;

    if (s == path) s++;

    *s = '\0';
}

void print_dir()
{
    uint32_t count;
    DirNode  fdir[256];
    xapi_SearchFile(current_path, &count, fdir);
    print_to_console_zh(shell_tr("文件名           类型   大小", "Name             Type   Size"));
    print_to_console("==============================================");
    if (count == 0 || count == 404) return;
    for (int i = 0; i < count; i++)
    {
        xapi_DrawSWText(handle, *ab_x, *ab_y, fdir[i].filename, TERMINAL_TEXT_COLOR);
        xapi_DrawSWText(handle,
                        *ab_x + 17 * 9,
                        *ab_y,
                        fdir[i].filetype ? shell_tr("文件夹", "Folder") : shell_tr("文件", "File"),
                        TERMINAL_TEXT_COLOR);
        if (fdir[i].filetype == 0)
        {
            xapi_DrawSWText(handle, *ab_x + 24 * 9, *ab_y, xcr_int2char(fdir[i].length), TERMINAL_TEXT_COLOR);
        }
        newline();
    }
}

int check_dir_com(char *str)
{
    uint32_t count;
    DirNode  fdir[256];
    xapi_SearchFile(current_path, &count, fdir);
    for (int i = 0; i < count; i++)
    {
        if (strcmp(fdir[i].filename, str) == 0)
        {
            if (fdir[i].filetype != 1) { return 1; }
            else { return 2; }
        }
    }
    return 0;
}

void create_directory(char *path)
{
    char  full_path[512];
    bool  recursive   = false;
    char *actual_path = path;

    if (strncmp(path, "-p ", 3) == 0)
    {
        recursive   = true;
        actual_path = path + 3;
        while (*actual_path == ' ')
            actual_path++;
    }

    if (actual_path[0] == '/')
    {
        strcpy(full_path, actual_path);
    }
    else
    {
        strcpy(full_path, current_path);
        if (full_path[strlen(full_path) - 1] != '/') { strcat(full_path, "/"); }
        strcat(full_path, actual_path);
    }

    normalize_path(full_path);

    if (recursive)
    {
        if (create_directory_recursive(full_path))
        {
            char success_msg[256];
            snprintf(success_msg, sizeof(success_msg), shell_tr("已创建目录: %s", "Created directory: %s"), full_path);
            print_to_console(success_msg);
        }
        else
        {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), shell_tr("mkdir：创建目录失败 '%s'", "mkdir: failed to create directory '%s'"), full_path);
            print_to_console(error_msg);
        }
    }
    else
    {
        if (xapi_Mkdir(full_path) == 0)
        {
            char success_msg[256];
            snprintf(success_msg, sizeof(success_msg), shell_tr("已创建目录: %s", "Created directory: %s"), full_path);
            print_to_console(success_msg);
        }
        else
        {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), shell_tr("mkdir：创建目录失败 '%s'", "mkdir: failed to create directory '%s'"), full_path);
            print_to_console(error_msg);
        }
    }
}

bool create_directory_recursive(char *path)
{
    char  temp_path[512] = "";
    char *token;
    char *rest = path;

    if (path[0] == '/')
    {
        strcat(temp_path, "/");
        rest = path + 1;
    }

    token = strtok(rest, "/");
    while (token != NULL)
    {
        if (strlen(temp_path) > 0 && temp_path[strlen(temp_path) - 1] != '/') { strcat(temp_path, "/"); }
        strcat(temp_path, token);

        uint32_t count;
        DirNode  fdir[256];
        xapi_SearchFile(temp_path, &count, fdir);
        int result = count;

        if (result == 404)
        {
            if (xapi_Mkdir(temp_path) != 0) { return false; }
        }

        token = strtok(NULL, "/");
    }

    return true;
}

void change_directory(char *path)
{
    char new_path[512];

    if (path == NULL || *path == '\0') path = (char *)"/";

    if (path[0] == '/')
    {
        snprintf(new_path, sizeof(new_path), "%s", path);
    }
    else
    {
        snprintf(new_path, sizeof(new_path), "%s%s%s", current_path,
                 current_path[strlen(current_path) - 1] == '/' ? "" : "/", path);
    }

    normalize_path(new_path);

    int ret = chdir(new_path);
    if (ret < 0)
    {
        char error_msg[256];
        if (-ret == ENOTDIR) {
            snprintf(error_msg, sizeof(error_msg), shell_tr("cd：不是目录：%s", "cd: not a directory: %s"), path);
        } else if (-ret == ENOENT) {
            snprintf(error_msg, sizeof(error_msg), shell_tr("cd：目录不存在：%s", "cd: directory not found: %s"), path);
        } else {
            snprintf(error_msg, sizeof(error_msg), shell_tr("cd：切换目录失败：%s", "cd: failed to change directory: %s"), path);
        }
        print_to_console(error_msg);
        return;
    }

    if (getcwd(current_path, sizeof(current_path)) == NULL) { strcpy(current_path, new_path); }
}

void normalize_path(char *path)
{
    char parts[50][50];
    int  part_count  = 0;
    char result[512] = "/";

    char *token = strtok(path, "/");
    while (token != NULL && part_count < 50)
    {
        if (strcmp(token, "..") == 0)
        {
            if (part_count > 0) { part_count--; }
        }
        else if (strcmp(token, ".") == 0 || strcmp(token, "") == 0)
        {
            // 忽略
        }
        else
        {
            strcpy(parts[part_count], token);
            part_count++;
        }
        token = strtok(NULL, "/");
    }

    for (int i = 0; i < part_count; i++)
    {
        if (i > 0) { strcat(result, "/"); }
        strcat(result, parts[i]);
    }

    if (strcmp(result, "") == 0) { strcpy(result, "/"); }
    strcpy(path, result);
}

void print_file_content(char *filename)
{
    char full_path[512];

    if (filename[0] == '/') { strcpy(full_path, filename); }
    else
    {
        strcpy(full_path, current_path);
        if (full_path[strlen(full_path) - 1] != '/') strcat(full_path, "/");
        strcat(full_path, filename);
    }

    char buffer[4096];
    int  ret = xapi_ReadFile(full_path, buffer, sizeof(buffer) - 1, 0);

    if (ret < 0)
    {
        char error_msg[256];
        snprintf(error_msg,
                 sizeof(error_msg),
                 shell_tr("cat：无法读取 '%s'：文件或目录不存在", "cat: cannot read '%s': file or directory not found"),
                 filename);
        print_to_console(error_msg);
        return;
    }

    buffer[ret] = '\0';

    char *line = buffer;
    char *next_line;
    while ((next_line = strchr(line, '\n')) != NULL)
    {
        *next_line = '\0';
        print_to_console(line);
        line = next_line + 1;
    }

    if (*line != '\0') { print_to_console(line); }
}

void create_file(char *filename)
{
    char full_path[512];

    if (filename[0] == '/') { strcpy(full_path, filename); }
    else
    {
        strcpy(full_path, current_path);
        if (full_path[strlen(full_path) - 1] != '/') strcat(full_path, "/");
        strcat(full_path, filename);
    }

    int ret = xapi_CreateFile(full_path);

    if (ret < 0)
    {
        char error_msg[256];
        snprintf(error_msg,
                 sizeof(error_msg),
                 shell_tr("touch：无法创建 '%s'：文件已存在或权限不足",
                          "touch: cannot create '%s': file exists or permission denied"),
                 filename);
        print_to_console(error_msg);
    }
    else
    {
        char success_msg[256];
        snprintf(success_msg, sizeof(success_msg), shell_tr("已创建文件: %s", "Created file: %s"), filename);
        print_to_console(success_msg);
    }
}

void remove_file_or_directory(char *path)
{
    char full_path[512];

    if (path[0] == '/') { strcpy(full_path, path); }
    else
    {
        strcpy(full_path, current_path);
        if (full_path[strlen(full_path) - 1] != '/') strcat(full_path, "/");
        strcat(full_path, path);
    }

    uint32_t count;
    DirNode  fdir[256];
    xapi_SearchFile(full_path, &count, fdir);

    if (count == 404)
    {
        char error_msg[256];
        snprintf(error_msg,
                 sizeof(error_msg),
                 shell_tr("rm：无法删除 '%s'：文件或目录不存在", "rm: cannot remove '%s': file or directory not found"),
                 path);
        print_to_console(error_msg);
        return;
    }

    int ret = xapi_DeleteFile(full_path);

    if (ret < 0)
    {
        char error_msg[256];
        snprintf(error_msg,
                 sizeof(error_msg),
                 shell_tr("rm：无法删除 '%s'：操作失败", "rm: cannot remove '%s': operation failed"),
                 path);
        print_to_console(error_msg);
    }
    else
    {
        char success_msg[256];
        snprintf(success_msg, sizeof(success_msg), shell_tr("已删除: %s", "Removed: %s"), path);
        print_to_console(success_msg);
    }
}

void copy_file(char *src, char *dst)
{
    char src_path[512], dst_path[512];

    if (src[0] == '/') { strcpy(src_path, src); }
    else
    {
        strcpy(src_path, current_path);
        if (src_path[strlen(src_path) - 1] != '/') strcat(src_path, "/");
        strcat(src_path, src);
    }

    if (dst[0] == '/') { strcpy(dst_path, dst); }
    else
    {
        strcpy(dst_path, current_path);
        if (dst_path[strlen(dst_path) - 1] != '/') strcat(dst_path, "/");
        strcat(dst_path, dst);
    }

    char buffer[8192];
    int  bytes_read = xapi_ReadFile(src_path, buffer, sizeof(buffer), 0);

    if (bytes_read < 0)
    {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), shell_tr("cp：无法复制 '%s'：文件不存在", "cp: cannot copy '%s': file not found"), src);
        print_to_console(error_msg);
        return;
    }

    int bytes_written = xapi_WriteFile(dst_path, buffer, bytes_read, 0);

    if (bytes_written < 0)
    {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), shell_tr("cp：无法创建 '%s'：权限不足", "cp: cannot create '%s': permission denied"), dst);
        print_to_console(error_msg);
    }
    else
    {
        char success_msg[256];
        snprintf(success_msg, sizeof(success_msg), shell_tr("已复制 '%s' 到 '%s'", "Copied '%s' to '%s'"), src, dst);
        print_to_console(success_msg);
    }
}

void move_file(char *src, char *dst)
{
    int ret = xapi_RenameFile(src, dst);

    if (ret < 0)
    {
        char error_msg[256];
        snprintf(error_msg,
                 sizeof(error_msg),
                 shell_tr("mv：无法将 '%s' 移动到 '%s'：操作失败", "mv: cannot move '%s' to '%s': operation failed"),
                 src,
                 dst);
        print_to_console(error_msg);
    }
    else
    {
        char success_msg[256];
        snprintf(success_msg, sizeof(success_msg), shell_tr("已将 '%s' 移动到 '%s'", "Moved '%s' to '%s'"), src, dst);
        print_to_console(success_msg);
    }
}

void print_working_directory()
{
    char pwd_msg[512];
    snprintf(pwd_msg, sizeof(pwd_msg), shell_tr("当前目录: %s", "Current directory: %s"), current_path);
    print_to_console(pwd_msg);
}
