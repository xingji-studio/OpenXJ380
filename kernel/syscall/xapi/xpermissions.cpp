#include <syscall/xapi_user.h>
#include <user/permissions.h>
#include <krlibc.h>
#include <errno.h>

uint64_t do_xapi_QueryFileAccess(uint64_t path, uint64_t access)
{
    char *copy = NULL;
    int result = xapi_copy_string_from_user(&copy, (const char *)path, XAPI_USER_PATH_MAX);
    if (result < 0) return (uint64_t)result;
    result = permissions_query_file_access(copy, access);
    free(copy);
    return (uint64_t)result;
}

uint64_t do_xapi_RequestFileAccess(uint64_t path, uint64_t access, uint64_t offered)
{
    if (offered & ~XPERM_ALL) return (uint64_t)-EINVAL;
    char *copy = NULL;
    int result = xapi_copy_string_from_user(&copy, (const char *)path, XAPI_USER_PATH_MAX);
    if (result < 0) return (uint64_t)result;
    // A caller can exit while the consent dialog is waiting. Keep the stable
    // pathname on its kernel stack instead of stranding a heap allocation.
    char stable[XAPI_USER_PATH_MAX];
    memcpy(stable, copy, strlen(copy) + 1);
    free(copy);
    return (uint64_t)permissions_request_file_access(stable, access, (uint8_t)offered);
}
