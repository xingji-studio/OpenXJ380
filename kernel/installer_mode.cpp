#include <installer_mode.h>

static bool g_installer_root_is_tmpfs_ready = false;

bool installer_root_is_tmpfs_ready()
{
    return g_installer_root_is_tmpfs_ready;
}
