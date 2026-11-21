// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <libsys.h>
#include <permission_types.h>

static void output(const char *message)
{
    size_t length = 0;
    while (message[length])
        ++length;
    int64_t result = int64_t(enter_syscall(SYS_WRITE, 2, uint64_t(message), length, 0, 0, 0));
    if (result < 0) enter_syscall(XAPI_OUTPUT, uint64_t(message), 0, 0, 0, 0, 0);
}

int main(int argc, char *argv[], char *envp[])
{
    (void)envp;
    if (argc != 3)
    {
        output("Usage: mountiso.elf IMAGE_OR_DEVICE MOUNTPOINT\n"
               "       mountiso.elf -u MOUNTPOINT\n");
        return 2;
    }
    bool    unmount = argv[1][0] == '-' && argv[1][1] == 'u' && !argv[1][2];
    int64_t consent = int64_t(enter_syscall(XAPI_REQUEST_PERMISSIONS, XPERM_CS | XPERM_NV, 0, 0, 0, 0, 0));
    if (consent)
    {
        output("mountiso: settings and external-directory permissions are required\n");
        return 1;
    }
    int64_t result;
    if (unmount)
        result = int64_t(enter_syscall(SYS_UMOUNT2, uint64_t(argv[2]), 0, 0, 0, 0, 0));
    else
        result = int64_t(enter_syscall(SYS_MOUNT, uint64_t(argv[1]), uint64_t(argv[2]), uint64_t("iso9660"), 1, 0, 0));
    if (!result) return 0;
    const char *reason = "operation failed";
    switch (-result)
    {
    case EBUSY: reason = "mount point or source is in use"; break;
    case ENOENT: reason = "path does not exist"; break;
    case EACCES:
    case EPERM: reason = "permission denied"; break;
    case ENOMEM: reason = "not enough memory"; break;
    case EIO: reason = "invalid or unreadable ISO image"; break;
    case EINVAL:
    case ENODEV: reason = "invalid source or mount point"; break;
    case EOPNOTSUPP: reason = "unsupported ISO format"; break;
    }
    output(unmount ? "mountiso: unmount: " : "mountiso: mount: ");
    output(reason);
    output("\n");
    return 1;
}
