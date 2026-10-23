#pragma once
#include <input_method.h>

struct ImeRegistry
{
    uint32_t count, mode;
    bool     provisioned;
    char     active[XIME_ID_BYTES];
    XImeInfo entries[XIME_MAX_INSTALLED];
};
int  ime_config_load(ImeRegistry *registry);
int  ime_config_save(const ImeRegistry *registry);
bool ime_valid_id(const char *id);
