#include <device.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <installer_mode.h>
#include <krlibc.h>
#include <proto.hpp>
#include <task/pcb.h>
#include <user/user.h>

static const BOOT_CONFIG *g_installer_boot_config = NULL;
static vfs_node_t         g_installer_base_root = NULL;
static bool               g_installer_root_is_tmpfs_ready = false;

extern int tmpfs_setup();

bool installer_boot_active(const BOOT_CONFIG &boot_config)
{
    return (boot_config.boot_flags & BOOT_FLAG_INSTALLER) != 0 &&
           boot_config.installer_root_pak != 0 &&
           boot_config.installer_root_pak_size >= sizeof(xj380_pak_header) &&
           boot_config.system_payload_pak != 0 &&
           boot_config.system_payload_pak_size >= sizeof(xj380_pak_header);
}

static const uint8_t *installer_phys_blob(uint64_t phys, uint64_t size)
{
    if (phys == 0 || size == 0) return NULL;
    return (const uint8_t *)(phys + 0xffff800000000000ULL);
}

static int installer_mkdir_parent(const char *path)
{
    if (path == NULL || path[0] != '/') return -EINVAL;
    char temp[512];
    size_t len = strlen(path);
    if (len >= sizeof(temp)) return -ENAMETOOLONG;
    strcpy(temp, path);

    for (size_t i = 1; temp[i] != '\0'; i++)
    {
        if (temp[i] != '/') continue;
        temp[i] = '\0';
        if (strlen(temp) > 0) vfs_mkdir(temp);
        temp[i] = '/';
    }
    return EOK;
}

static int installer_write_file(const char *path, const uint8_t *data, size_t size)
{
    if (installer_mkdir_parent(path) < 0) return -EINVAL;
    vfs_mkfile(path);
    vfs_node_t node = vfs_open(path);
    if (node == NULL) return -ENOENT;
    if (size > 0 && vfs_write(node, (void *)data, 0, size) != size)
    {
        vfs_close(node);
        return -EIO;
    }
    vfs_close(node);
    return EOK;
}

static int installer_unpack_pak(const uint8_t *pak, size_t pak_size)
{
    if (pak == NULL || pak_size < sizeof(xj380_pak_header)) return -EINVAL;
    const xj380_pak_header *header = (const xj380_pak_header *)pak;
    if (memcmp(header->magic, XJ380_INSTALLER_PAK_MAGIC, XJ380_INSTALLER_PAK_MAGIC_SIZE) != 0) return -EINVAL;

    size_t off = sizeof(*header);
    for (uint64_t i = 0; i < header->entry_count; i++)
    {
        if (off > pak_size || pak_size - off < sizeof(xj380_pak_entry_header)) return -EINVAL;
        const xj380_pak_entry_header *entry = (const xj380_pak_entry_header *)(pak + off);
        off += sizeof(*entry);
        if (entry->path_len == 0 || entry->path_len >= 512 || off > pak_size || pak_size - off < entry->path_len)
            return -EINVAL;

        char path[512];
        memcpy(path, pak + off, entry->path_len);
        path[entry->path_len] = '\0';
        off += entry->path_len;
        if (path[0] != '/') return -EINVAL;

        if (entry->type == XJ380_PAK_ENTRY_DIR)
        {
            vfs_mkdir(path);
        }
        else if (entry->type == XJ380_PAK_ENTRY_FILE)
        {
            if (off > pak_size || entry->size > pak_size - off) return -EINVAL;
            int ret = installer_write_file(path, pak + off, (size_t)entry->size);
            if (ret < 0) return ret;
            off += (size_t)entry->size;
        }
        else
        {
            return -EINVAL;
        }
    }
    return EOK;
}

int installer_prepare_root(const BOOT_CONFIG &boot_config)
{
    g_installer_boot_config = &boot_config;
    g_installer_base_root = rootdir;

    int ret = tmpfs_setup();
    if (ret < 0) return ret;
    vfs_node_t tmp_root = vfs_open("/tmp");
    if (tmp_root == NULL) return -ENOENT;
    set_rootdir(tmp_root);
    g_installer_root_is_tmpfs_ready = true;

    vfs_mkdir("/dev");
    vfs_mkdir("/tmp");
    vfs_mkdir("/apps");
    vfs_mkdir("/apps/system");
    vfs_mkdir("/apps/builtin");
    vfs_mkdir("/system");
    vfs_mkdir("/system/config");
    vfs_mkdir("/system/resources");
    vfs_mkdir("/system/resources/image");
    vfs_node_t dev = vfs_open("/dev");
    if (dev == NULL) return -ENOENT;
    if (vfs_mount((const char *)DEVFS_REGISTER_ID, dev) < 0) return -EIO;

    const uint8_t *root_pak = installer_phys_blob(boot_config.installer_root_pak,
                                                  boot_config.installer_root_pak_size);
    ret = installer_unpack_pak(root_pak, (size_t)boot_config.installer_root_pak_size);
    if (ret < 0) return ret;
    return EOK;
}

bool installer_root_is_tmpfs_ready()
{
    return g_installer_root_is_tmpfs_ready;
}

void installer_launch_app()
{
    if (current_user == NULL)
    {
        current_user = (UserInfo *)malloc(sizeof(UserInfo));
        if (current_user != NULL)
        {
            memset(current_user, 0, sizeof(UserInfo));
            strcpy(current_user->name, "Installer");
            current_user->user_type = XUT_Root;
            current_user->envp = root_user.envp;
            current_user->envc = root_user.envc;
        }
    }
    int pid = create_user_process_from_file((char *)"/apps/system/shell.elf", NULL, NULL);
    write_serial_fmt("installer: launch /apps/system/shell.elf pid=%d\n", pid);
}

const BOOT_CONFIG *installer_current_boot_config()
{
    return g_installer_boot_config;
}

vfs_node_t installer_base_root()
{
    return g_installer_base_root;
}
