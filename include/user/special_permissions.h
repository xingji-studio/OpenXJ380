#pragma once

#include <user/permissions.h>

// Called at authenticated login and lazily by the request API. VFS policy
// checks use only the in-memory snapshot, never nested filesystem I/O.
int special_permissions_load_user(const char *user);
int special_permissions_get(const char *user, const char *executable, uint64_t spid);
int special_permissions_save(const char *user, const char *executable, uint64_t spid, bool approved);
void special_permissions_store_exit(pcb_t process);

// 0 unknown, 1 refused, 2 granted. No allocation, VFS, waiting or modal UI.
int special_permissions_cached(const char *user, const char *executable, uint64_t spid);
uint64_t special_permissions_path_mask(const char *path, int operation, const char *user);
// 0 ordinary policy, 1 specially authorized, negative errno denied.
int special_permissions_access(pcb_t process, const char *user, const char *path, int operation);
bool special_permissions_store_path(const char *path, bool ancestor = false);

// Uses the ordinary permission service's single global consent FIFO.
int permissions_request_special(uint64_t spid);
