// SPDX-License-Identifier: Apache-2.0
#include <user/auth.h>
#include <user/auth_crypto.h>
#include <syscall/xapi_user.h>
#include <syscall/file_io_scope.h>

uint64_t do_xapi_AuthBegin(uint64_t request, uint64_t output)
{
    XAuthBegin input = {};
    XAuthChallenge challenge = {};
    page_directory_t *directory = xapi_current_pagedir();
    if (!copy_from_user_pagedir(directory, &input, (void *)request, sizeof(input)) ||
        !copy_to_user_pagedir(directory, (void *)output, &challenge, sizeof(challenge)))
    {
        auth_erase(&input, sizeof(input));
        return (uint64_t)-EFAULT;
    }
    int error = user_auth_begin(&input, &challenge);
    auth_erase(&input, sizeof(input));
    if (!error && !copy_to_user_pagedir(directory, (void *)output, &challenge, sizeof(challenge))) error = -EFAULT;
    return (uint64_t)(int64_t)error;
}

uint64_t do_xapi_AuthFinish(uint64_t request, uint64_t output)
{
    XAuthRequest input = {};
    XAuthResult result = {};
    page_directory_t *directory = xapi_current_pagedir();
    if (!copy_from_user_pagedir(directory, &input, (void *)request, sizeof(input)) ||
        !copy_to_user_pagedir(directory, (void *)output, &result, sizeof(result))) return (uint64_t)-EFAULT;
    unsigned operation = 0;
    int error = user_auth_finish(&input, &result, &operation);
    if (!error && !copy_to_user_pagedir(directory, (void *)output, &result, sizeof(result))) error = -EFAULT;
    volatile uint8_t *erase = result.grant;
    for (size_t i = 0; i < sizeof(result.grant); ++i) erase[i] = 0;
    return (uint64_t)(int64_t)error;
}

uint64_t do_xapi_AccountInfo(uint64_t output)
{
    XAccountInfo info = {};
    int error = user_auth_account_info(&info);
    if (!error && !copy_to_user_pagedir(xapi_current_pagedir(), (void *)output, &info, sizeof(info))) error = -EFAULT;
    return (uint64_t)(int64_t)error;
}
