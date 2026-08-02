#include "../build_settings.h"
#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <elf.h>
#include <errno.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <stdint.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/user.h>
#include <user/runfile.h>
#include <user/settings.h>

extern UserInfo *current_user;

static bool make_user_path(char *out, size_t out_size, const char *username, const char *leaf)
{
    if (out == NULL || username == NULL || leaf == NULL) return false;
    size_t need = strlen("/users/") + strlen(username) + strlen(leaf) + 1;
    if (need > out_size) return false;
    memset(out, 0, out_size);
    strcat(out, "/users/");
    strcat(out, username);
    strcat(out, leaf);
    return true;
}

bool init_user_dirs(const char *username)
{
    if (username == NULL || username[0] == '\0') return false;

    vfs_mkdir("/users");

    char user_folder_path[256];
    if (!make_user_path(user_folder_path, sizeof(user_folder_path), username, "")) return false;
    if (vfs_mkdir(user_folder_path) != EOK) return false;

    char desktop_path[256];
    if (!make_user_path(desktop_path, sizeof(desktop_path), username, "/desktop")) return false;
    vfs_mkdir(desktop_path);
    return true;
}

static bool write_file_if_empty(const char *path, const void *data, size_t size)
{
    if (path == NULL || data == NULL || size == 0) return false;

    vfs_node_t existing = vfs_open_no_follow(path);
    if (existing != NULL)
    {
        bool already_initialized = !(existing->type & file_dir) && existing->size > 0;
        vfs_close(existing);
        if (already_initialized) return true;
    }

    if (vfs_mkfile(path) != EOK) return false;
    vfs_node_t file = vfs_open(path);
    if (file == NULL) return false;

    vfs_resize(file, 0);
    size_t wrote = vfs_write(file, (void *)data, 0, size);
    vfs_close(file);
    return wrote == size;
}

static int normalize_settings_language(int language)
{
    return language == XJ380_LANGUAGE_EN_US ? XJ380_LANGUAGE_EN_US : XJ380_LANGUAGE_ZH_CN;
}

static int read_default_settings_language()
{
    int language = XJ380_LANGUAGE_ZH_CN;

    vfs_node_t seed = vfs_open(XJ380_LANGUAGE_SEED_PATH);
    if (seed == NULL) return language;

    char buffer[16];
    memset(buffer, 0, sizeof(buffer));
    size_t read_size = seed->size < sizeof(buffer) - 1 ? seed->size : sizeof(buffer) - 1;
    if (read_size > 0 && vfs_read(seed, buffer, 0, read_size) > 0)
    {
        if (buffer[0] == '1' || strncmp(buffer, "en", 2) == 0 || strncmp(buffer, "EN", 2) == 0)
            language = XJ380_LANGUAGE_EN_US;
    }

    vfs_close(seed);
    return normalize_settings_language(language);
}

bool init_user_runfile(const char *username)
{
    if (!init_user_dirs(username)) return false;

    char runfile_setfile_path[256];
    if (!make_user_path(runfile_setfile_path, sizeof(runfile_setfile_path), username, "/runfile.dat")) return false;

    char settings_file_buffer[sizeof(RunfileSettings_Format)];
    memset(settings_file_buffer, 0, sizeof(settings_file_buffer));
    RunfileSettings_Format *file_format = (RunfileSettings_Format *)settings_file_buffer;

    strcpy(file_format->items[0].exname,   "txt");
    strcpy(file_format->items[0].describe, "文本文件");
    strcpy(file_format->items[0].runpath,  "/apps/builtin/texter.elf");

    strcpy(file_format->items[1].exname,   "inf");
    strcpy(file_format->items[1].describe, "配置文件");
    strcpy(file_format->items[1].runpath,  "/apps/builtin/texter.elf");

    strcpy(file_format->items[2].exname,   "png");
    strcpy(file_format->items[2].describe, "PNG 图片文件");
    strcpy(file_format->items[2].runpath,  "/apps/builtin/picturer.elf");

    strcpy(file_format->items[3].exname,   "jpg");
    strcpy(file_format->items[3].describe, "JPEG 图片文件");
    strcpy(file_format->items[3].runpath,  "/apps/builtin/picturer.elf");

    strcpy(file_format->items[4].exname,   "bmp");
    strcpy(file_format->items[4].describe, "位图文件");
    strcpy(file_format->items[4].runpath,  "/apps/builtin/picturer.elf");

    strcpy(file_format->items[5].exname,   "xtb");
    strcpy(file_format->items[5].describe, "XJ380 块状文本标记文本文件");
    strcpy(file_format->items[5].runpath,  "/apps/builtin/texter.elf");

    return write_file_if_empty(runfile_setfile_path, settings_file_buffer, sizeof(settings_file_buffer));
}

bool init_user_settings(const char *username)
{
    if (!init_user_dirs(username)) return false;

    char settings_path[256];
    if (!make_user_path(settings_path, sizeof(settings_path), username, "/settings.dat")) return false;

    char settings_data_buffer[sizeof(SettingsDataFileFormat)];
    memset(settings_data_buffer, 0, sizeof(settings_data_buffer));
    SettingsDataFileFormat *file_data = (SettingsDataFileFormat *)settings_data_buffer;

    strcpy(file_data->BackgroundFilePath, "/system/resources/image/background2.png");
    file_data->ClockHourOffset = 8;
    file_data->Language = read_default_settings_language();

    return write_file_if_empty(settings_path, settings_data_buffer, sizeof(settings_data_buffer));
}

bool init_user_profile(const char *username)
{
    return init_user_dirs(username) && init_user_runfile(username) && init_user_settings(username);
}

void init_runfile()
{
    if (current_user == NULL) return;
    init_user_profile(current_user->name);
}

void runfile(char *path)
{
    // 获取扩展名
    char exname[10];
    memset(exname, 0, 10);
    if (!get_file_exname(path, exname))
    {
        return;
    }

    if (strcmp("elf", exname) == 0 || strcmp("epf", exname) == 0)
    {
        // 是可执行文件
        create_user_process_from_file(path, NULL, NULL);
        return;
    }

    // 搜索扩展名
    char runfile_setfile_path[256];
    memset(runfile_setfile_path, 0, 256);
    strcat(runfile_setfile_path, "/users/");
    strcat(runfile_setfile_path, current_user->name);
    strcat(runfile_setfile_path, "/runfile.dat");
    vfs_mkfile(runfile_setfile_path);

    vfs_node_t settings_file_v = vfs_open(runfile_setfile_path);
    if (!settings_file_v) return;
    size_t settings_size = settings_file_v->size;
    if (settings_size > sizeof(RunfileSettings_Format))
    {
        vfs_close(settings_file_v);
        return;
    }

    RunfileSettings_Format *file_format = (RunfileSettings_Format *)calloc(1, sizeof(RunfileSettings_Format));
    if (file_format == NULL)
    {
        vfs_close(settings_file_v);
        return;
    }
    size_t bytes_read = vfs_read(settings_file_v, file_format, 0, settings_size);
    vfs_close(settings_file_v);
    if (bytes_read != settings_size)
    {
        free(file_format);
        return;
    }

    for (int i = 0; i < 1024; i++)
    {
        RunfileSettings_Item *item = &file_format->items[i];
        if (memchr(item->exname, '\0', sizeof(item->exname)) == NULL ||
            memchr(item->runpath, '\0', sizeof(item->runpath)) == NULL)
            continue;
        if (strcmp(exname, item->exname) == 0)
        {
            // 是可执行文件
            char *argv[2]    = {path, NULL};
            create_user_process_from_file(item->runpath, NULL, argv);
            free(file_format);
            return;
        }
    }

    free(file_format);
}

bool get_file_exname(char *name, char *exname)
{
    if (name == NULL || exname == NULL) return false;

    char *p = name;
    char *q = exname;
    bool have_type = false;
    while (*p)
    {
        p++;
        if (*p == '.')
            have_type = true;
    }

    if (!have_type)
        return false;
    
    while (*p != '.')
        p--;

    p++;

    size_t copied = 0;
    while (*p)
    {
        if (copied >= 9) break;
        *q = *p;
        p++; q++;
        copied++;
    }
    *q = '\0';
    
    return true;
}
