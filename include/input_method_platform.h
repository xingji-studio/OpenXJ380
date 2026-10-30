#pragma once
#include <input_method.h>

// Kernel/module interface only. User programs cannot install a platform or
// choose their identity, executable paths, storage root or option privileges.
struct ImeRegistry
{
    uint32_t count, mode;
    bool     provisioned;
    char     active[XIME_ID_BYTES];
    XImeInfo entries[XIME_MAX_INSTALLED];
};

enum ImePathAccess
{
    IME_PATH_PRIVATE = 1,   // No direct application reads or writes.
    IME_PATH_READ_ONLY = 2 // Executables remain readable but immutable to apps.
};

struct ImePathRule
{
    const char *path; // Absolute path; '*' matches one complete component.
    uint32_t    access;
};

struct ImeProviderPolicy
{
    const char *id, *executable, *settings;
    const char *data_root; // Optional private root; otherwise use account storage.
    uint32_t    option_mask, default_flags; // Opaque preferences, except ENABLED.
};

struct ImePlatform
{
    uint32_t size, version;
    const char *manager;
    const char *account_store; // Private directory template with one '*' account component.
    const ImeProviderPolicy *providers;
    uint32_t provider_count;
    const ImePathRule *paths;
    uint32_t path_count;
    int (*load)(ImeRegistry *);
    int (*save)(const ImeRegistry *);
};

// Called once during trusted module initialization, before launching apps.
// The immutable table and callbacks must remain resident for the boot lifetime.
// Installation does no allocation, VFS access or blocking, including at boot.
int ime_install_platform(const ImePlatform *platform);
bool ime_valid_id(const char *id);
