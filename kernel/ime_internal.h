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

struct ImeUserIdentity
{
    char user[64], id[XIME_ID_BYTES];
};
int ime_user_authorize(const XImeUserData *request, ImeUserIdentity *identity);
int64_t ime_user_store_syscall(uint64_t address);
void ime_user_store_initialize();
void ime_user_store_process_exit(pcb_t process);
