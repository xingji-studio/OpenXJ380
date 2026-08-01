#pragma once

#include <stdint.h>

#define XJ380_LANGUAGE_ZH_CN 0
#define XJ380_LANGUAGE_EN_US 1
#define XJ380_LANGUAGE_SEED_PATH "/system/config/language.dat"

struct SettingsDataFileFormat
{
    char    BackgroundFilePath[256];
    int     ClockHourOffset;
    int     Language;
};
