#include <errno.h>
#include <syscall/pxapi.h>

void do_xapi_Broken(char *info)
{
    (void)info;
}

uint64_t do_xapi_SendAppMessage(char *title, char *text)
{
    (void)title;
    (void)text;
    return (uint64_t)-ENOSYS;
}

uint64_t do_xapi_InstallerEnumDisks(uint64_t list) { (void)list; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerStart(uint64_t disk_id) { (void)disk_id; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerStartEx(uint64_t disk_id, uint64_t mode) { (void)disk_id; (void)mode; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerStartOptions(uint64_t options) { (void)options; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerPrecheck(uint64_t disk_id, uint64_t mode, uint64_t out) { (void)disk_id; (void)mode; (void)out; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerPrecheckOptions(uint64_t options, uint64_t out) { (void)options; (void)out; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerProgress(uint64_t progress) { (void)progress; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerRescue(uint64_t action, uint64_t disk_id, uint64_t out) { (void)action; (void)disk_id; (void)out; return (uint64_t)-ENOSYS; }
uint64_t do_xapi_InstallerLog(uint64_t out) { (void)out; return (uint64_t)-ENOSYS; }
