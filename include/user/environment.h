#pragma once

#include <stdint.h>

// These limits apply to files and explicit native launch environments alike.
static constexpr size_t ENVIRONMENT_FILE_MAX = 65536;
static constexpr size_t ENVIRONMENT_ENTRY_MAX = 4096;
static constexpr size_t ENVIRONMENT_COUNT_MAX = 256;
static constexpr unsigned ENVIRONMENT_SYNC_DEFAULT = 120;
static constexpr unsigned ENVIRONMENT_SYNC_MIN = 1;
static constexpr unsigned ENVIRONMENT_SYNC_MAX = 86400;

struct EnvironmentTable
{
    char **values;
    size_t count;
    size_t bytes;
};

bool environment_name_valid(const char *name, size_t length);
void environment_free(char **values);
int environment_set(EnvironmentTable *table, const char *entry);
int environment_parse(EnvironmentTable *table, const char *text, size_t length, size_t *error_line);
char **environment_clone(char *const *values, size_t *count);
bool environment_elf_native_entry(const void *image, size_t size);

// Returns an owned, NULL-terminated copy. Never retains caller/user pointers.
char **environment_for_user(const char *username, size_t *count);
int environment_reload(const char *username);
bool environment_init_profile(const char *username);
void environment_start_worker();
