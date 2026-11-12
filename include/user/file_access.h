#pragma once

enum
{
    XFA_READ = 1,
    XFA_WRITE = 2,
    XFA_LIST = 4,
    XFA_STAT = 8,
    XFA_CREATE = 16,
    XFA_REMOVE = 32,
    XFA_NOFOLLOW = 64,
    XFA_EXCLUSIVE = 128,
    // Read data exported from a directory, rather than only its listing.
    XFA_EXPORT = 256
};
