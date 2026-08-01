#pragma once

#include <graphics/sheet.h>

#ifdef __cplusplus
extern "C" {
#endif

int xapi_drawSvgBySheet(
    SHEET_INFO *sheetInfo,
    SHEET      *sheet,
    int         startX,
    int         startY,
    int         width,
    const char *svgText,
    bool        enableTrans = false
);

int xapi_drawFABySheet(
    SHEET_INFO *sheetInfo,
    SHEET      *sheet,
    int         startX,
    int         startY,
    int         width,
    const char *name,
    bool        enableTrans = false
);

#ifdef __cplusplus
}
#endif
