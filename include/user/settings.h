#pragma once

#include <stdint.h>

#include "language.h"
#define XJ380_LANGUAGE_SEED_PATH "/system/config/language.dat"

// settings.json stores the exclusive maximum interval between clicks in milliseconds.
#define XJ380_MOUSE_DOUBLE_CLICK_DEFAULT_MS 1000
#define XJ380_MOUSE_DOUBLE_CLICK_MIN_MS 100
#define XJ380_MOUSE_DOUBLE_CLICK_MAX_MS 2000

struct SettingsDataFileFormat
{
    char    BackgroundFilePath[256];
    int     ClockHourOffset;
    int     Language;
};
