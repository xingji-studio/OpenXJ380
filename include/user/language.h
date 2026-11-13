#pragma once

// Persisted IDs: retain 0/1 for existing settings and never reorder these values.
#define XJ380_LANGUAGE_ZH_CN 0
#define XJ380_LANGUAGE_EN_US 1
#define XJ380_LANGUAGE_ZH_MS 2
#define XJ380_LANGUAGE_ZH_TW 3
#define XJ380_LANGUAGE_JA_JP 4
#define XJ380_LANGUAGE_FR    5
#define XJ380_LANGUAGE_DE    6
#define XJ380_LANGUAGE_COUNT 7

static inline int xj380_language_normalize(int language)
{
    return language >= 0 && language < XJ380_LANGUAGE_COUNT ? language : XJ380_LANGUAGE_ZH_CN;
}

static inline int xj380_language_from_number(const double *number)
{
    // Read IEEE binary64 storage using integer operations. Some freestanding
    // applications use -mgeneral-regs-only and cannot call a floating-point ABI.
    if (!number) return XJ380_LANGUAGE_ZH_CN;
    const unsigned char *bytes = (const unsigned char *)number;
    unsigned long long bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= (unsigned long long)bytes[i] << (8 * i);
    static const unsigned long long encoded[XJ380_LANGUAGE_COUNT] = {
        0, 0x3ff0000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL,
        0x4010000000000000ULL, 0x4014000000000000ULL, 0x4018000000000000ULL,
    };
    for (int language = 0; language < XJ380_LANGUAGE_COUNT; ++language)
        if (bits == encoded[language]) return language;
    return XJ380_LANGUAGE_ZH_CN;
}

static inline const char *xj380_language_code(int language)
{
    static const char *const codes[XJ380_LANGUAGE_COUNT] = {"zh_CN", "en_US", "zh_MS", "zh_TW", "ja_JP", "fr", "de"};
    return codes[xj380_language_normalize(language)];
}

static inline const char *xj380_language_name(int language)
{
    static const char *const names[XJ380_LANGUAGE_COUNT] = {
        "简体中文", "English (United States)", "微软式中文", "繁體中文（中國台灣）", "日本語", "Français", "Deutsch",
    };
    return names[xj380_language_normalize(language)];
}

static inline int xj380_language_from_code(const char *code)
{
    if (!code) return XJ380_LANGUAGE_ZH_CN;
    // Preserve old seed files containing a numeric ID or the short zh/en code.
    if (code[0] >= '0' && code[0] < '0' + XJ380_LANGUAGE_COUNT &&
        (!code[1] || code[1] == '\n' || code[1] == '\r')) return code[0] - '0';
    for (int language = 0; language < XJ380_LANGUAGE_COUNT; ++language)
    {
        const char *expected = xj380_language_code(language);
        unsigned i = 0;
        while (expected[i] && code[i])
        {
            char actual = code[i] == '-' ? '_' : code[i];
            char wanted = expected[i];
            if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
            if (wanted >= 'A' && wanted <= 'Z') wanted += 'a' - 'A';
            if (actual != wanted) break;
            ++i;
        }
        if ((!expected[i] || (i == 2 && language <= XJ380_LANGUAGE_EN_US)) &&
            (!code[i] || code[i] == '\n' || code[i] == '\r')) return language;
    }
    return XJ380_LANGUAGE_ZH_CN;
}
