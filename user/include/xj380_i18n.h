#pragma once

#include <x3api.h>
#include <user/settings.h>

static inline int xj380_normalize_language(int language)
{
    return language == XJ380_LANGUAGE_EN_US ? XJ380_LANGUAGE_EN_US : XJ380_LANGUAGE_ZH_CN;
}

static inline int xj380_read_language()
{
    int language = XJ380_LANGUAGE_ZH_CN;

    UserInfo user;
    memset(&user, 0, sizeof(user));
    xapi_GetCurrentUser(&user);
    if (user.name[0] == '\0') return language;

    char path[256];
    memset(path, 0, sizeof(path));
    snprintf(path, sizeof(path), "/users/%s/settings.dat", user.name);

    XFILE *file = xapi_OpenFile(path);
    if (file == NULL || file->buffer == NULL)
    {
        if (file != NULL) xapi_CloseFile(file);
        return language;
    }

    if (file->length >= sizeof(SettingsDataFileFormat))
    {
        SettingsDataFileFormat *settings = (SettingsDataFileFormat *)file->buffer;
        language = settings->Language;
    }

    xapi_CloseFile(file);
    return xj380_normalize_language(language);
}

static inline char *xj380_tr_lang(int language, const char *zh_cn, const char *en_us)
{
    return (char *)(xj380_normalize_language(language) == XJ380_LANGUAGE_EN_US ? en_us : zh_cn);
}

static inline char *xj380_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(xj380_read_language(), zh_cn, en_us);
}
