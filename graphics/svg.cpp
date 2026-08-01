#include <graphics/svg.h>

#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/alloc/alloc.h>
#include <xapi_xml.h>

typedef struct xapi_style
{
    bool     fillEnabled;
    uint32_t fillColor;
    bool     fillRuleEvenOdd;
    bool     strokeEnabled;
    uint32_t strokeColor;
    int      strokeWidth;
} xapi_style;

typedef struct xapi_renderContext
{
    SHEET_INFO *sheetInfo;
    SHEET      *sheet;
    int         startX;
    int         startY;
    double      viewBoxMinX;
    double      viewBoxMinY;
    double      scale;
    int         outputWidth;
    int         outputHeight;
    bool        enableTrans;
} xapi_renderContext;

typedef struct xapi_pathPointList
{
    int *xs;
    int *ys;
    int  count;
    int  capacity;
} xapi_pathPointList;

typedef struct xapi_fillContour
{
    int *xs;
    int *ys;
    int  count;
} xapi_fillContour;

typedef struct xapi_fillContourList
{
    xapi_fillContour *items;
    int               count;
    int               capacity;
} xapi_fillContourList;

typedef struct xapi_scanIntersection
{
    int x;
    int windingDelta;
} xapi_scanIntersection;

static uint32_t xapi_packColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

static SHEET_BUFFER xapi_toSheetColor(uint32_t color)
{
    SHEET_BUFFER out;
    out.r = (uint8_t)((color >> 24) & 0xff);
    out.g = (uint8_t)((color >> 16) & 0xff);
    out.b = (uint8_t)((color >> 8) & 0xff);
    out.a = (uint8_t)(color & 0xff);
    return out;
}

static uint32_t xapi_applyTransColor(const xapi_renderContext *ctx, uint32_t color)
{
    if (!ctx || !ctx->enableTrans) { return color; }
    uint8_t r = (uint8_t)((color >> 24) & 0xff);
    uint8_t g = (uint8_t)((color >> 16) & 0xff);
    uint8_t b = (uint8_t)((color >> 8) & 0xff);
    uint8_t a = (uint8_t)(color & 0xff);
    return xapi_packColor((uint8_t)(0xff - r), (uint8_t)(0xff - g), (uint8_t)(0xff - b), a);
}

static int xapi_roundToInt(double value)
{
    return (value >= 0.0) ? (int)(value + 0.5) : (int)(value - 0.5);
}

static double xapi_absDouble(double value)
{
    return value < 0.0 ? -value : value;
}

static double xapi_sqrtDouble(double value)
{
    if (value <= 0.0) { return 0.0; }

    double x = value > 1.0 ? value : 1.0;
    for (int i = 0; i < 24; i++)
    {
        x = 0.5 * (x + value / x);
    }
    return x;
}

static double xapi_wrapAngle(double angle)
{
    const double pi    = 3.14159265358979323846;
    const double twoPi = 6.28318530717958647692;

    while (angle > pi)
    {
        angle -= twoPi;
    }
    while (angle < -pi)
    {
        angle += twoPi;
    }
    return angle;
}

static double xapi_sinApprox(double angle)
{
    angle = xapi_wrapAngle(angle);
    double x2 = angle * angle;
    double x3 = x2 * angle;
    double x5 = x3 * x2;
    double x7 = x5 * x2;
    double x9 = x7 * x2;

    return angle - x3 / 6.0 + x5 / 120.0 - x7 / 5040.0 + x9 / 362880.0;
}

static double xapi_cosApprox(double angle)
{
    angle = xapi_wrapAngle(angle);
    double x2 = angle * angle;
    double x4 = x2 * x2;
    double x6 = x4 * x2;
    double x8 = x6 * x2;

    return 1.0 - x2 / 2.0 + x4 / 24.0 - x6 / 720.0 + x8 / 40320.0;
}

static double xapi_atanApprox(double value)
{
    const double piDiv2 = 1.57079632679489661923;
    double       absV   = xapi_absDouble(value);

    if (absV <= 1.0)
    {
        return value * (0.7853981633974483 + 0.273 * (1.0 - absV));
    }

    double inv  = 1.0 / value;
    double atan = inv * (0.7853981633974483 + 0.273 * (1.0 - xapi_absDouble(inv)));
    return value > 0.0 ? (piDiv2 - atan) : (-piDiv2 - atan);
}

static double xapi_atan2Approx(double y, double x)
{
    const double pi     = 3.14159265358979323846;
    const double piDiv2 = 1.57079632679489661923;

    if (x > 0.0) { return xapi_atanApprox(y / x); }
    if (x < 0.0)
    {
        if (y >= 0.0) { return xapi_atanApprox(y / x) + pi; }
        return xapi_atanApprox(y / x) - pi;
    }
    if (y > 0.0) { return piDiv2; }
    if (y < 0.0) { return -piDiv2; }
    return 0.0;
}

static int xapi_clampInt(int value, int minValue, int maxValue)
{
    if (value < minValue) { return minValue; }
    if (value > maxValue) { return maxValue; }
    return value;
}

static double xapi_transformXf(const xapi_renderContext *ctx, double svgX)
{
    return (double)ctx->startX + (svgX - ctx->viewBoxMinX) * ctx->scale;
}

static double xapi_transformYf(const xapi_renderContext *ctx, double svgY)
{
    return (double)ctx->startY + (svgY - ctx->viewBoxMinY) * ctx->scale;
}

static int xapi_transformX(const xapi_renderContext *ctx, double svgX)
{
    return xapi_roundToInt(xapi_transformXf(ctx, svgX));
}

static int xapi_transformY(const xapi_renderContext *ctx, double svgY)
{
    return xapi_roundToInt(xapi_transformYf(ctx, svgY));
}

static void xapi_drawScreenPointWithWidth(
    xapi_renderContext *ctx,
    int                                    x,
    int                                    y,
    uint32_t                               color,
    int                                    width)
{
    int          left  = (width - 1) / 2;
    int          right = width / 2;
    SHEET_BUFFER pixel = xapi_toSheetColor(xapi_applyTransColor(ctx, color));
    for (int dy = -left; dy <= right; dy++)
    {
        for (int dx = -left; dx <= right; dx++)
        {
            draw_point(ctx->sheetInfo, ctx->sheet, x + dx, y + dy, pixel);
        }
    }
}

static void xapi_drawScreenLineWithWidth(
    xapi_renderContext *ctx,
    int                                    x0,
    int                                    y0,
    int                                    x1,
    int                                    y1,
    uint32_t                               color,
    int                                    width)
{
    int dx = ABS(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -ABS(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;)
    {
        xapi_drawScreenPointWithWidth(ctx, x0, y0, color, width);
        if (x0 == x1 && y0 == y1) { break; }
        int e2 = err * 2;
        if (e2 >= dy)
        {
            err += dy;
            x0  += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0  += sy;
        }
    }
}

static void xapi_drawSvgLine(
    xapi_renderContext *ctx,
    double                                 x0,
    double                                 y0,
    double                                 x1,
    double                                 y1,
    uint32_t                               color,
    int                                    width)
{
    int sx0 = xapi_transformX(ctx, x0);
    int sy0 = xapi_transformY(ctx, y0);
    int sx1 = xapi_transformX(ctx, x1);
    int sy1 = xapi_transformY(ctx, y1);
    xapi_drawScreenLineWithWidth(ctx, sx0, sy0, sx1, sy1, color, width);
}

static void xapi_skipDelimiters(const char **cursor)
{
    while (**cursor)
    {
        char ch = **cursor;
        if (ch == ',' || isspace((unsigned char)ch))
        {
            (*cursor)++;
            continue;
        }
        break;
    }
}

static bool xapi_parseDouble(const char **cursor, double *outValue)
{
    const char *p = *cursor;
    xapi_skipDelimiters(&p);

    int sign = 1;
    if (*p == '+')
    {
        p++;
    }
    else if (*p == '-')
    {
        sign = -1;
        p++;
    }

    bool   hasDigit = false;
    double value    = 0.0;
    while (*p >= '0' && *p <= '9')
    {
        hasDigit = true;
        value    = value * 10.0 + (double)(*p - '0');
        p++;
    }

    if (*p == '.')
    {
        p++;
        double factor = 0.1;
        while (*p >= '0' && *p <= '9')
        {
            hasDigit = true;
            value += (double)(*p - '0') * factor;
            factor *= 0.1;
            p++;
        }
    }

    if (!hasDigit) { return false; }

    if (*p == 'e' || *p == 'E')
    {
        const char *expCursor = p + 1;
        int         expSign   = 1;
        if (*expCursor == '+')
        {
            expCursor++;
        }
        else if (*expCursor == '-')
        {
            expSign = -1;
            expCursor++;
        }

        if (*expCursor >= '0' && *expCursor <= '9')
        {
            int exponent = 0;
            while (*expCursor >= '0' && *expCursor <= '9')
            {
                exponent = exponent * 10 + (int)(*expCursor - '0');
                expCursor++;
            }
            if (exponent > 308) { exponent = 308; }
            if (expSign > 0)
            {
                while (exponent-- > 0)
                {
                    value *= 10.0;
                }
            }
            else
            {
                while (exponent-- > 0)
                {
                    value *= 0.1;
                }
            }
            p = expCursor;
        }
    }

    *outValue = value * (double)sign;
    *cursor   = p;
    return true;
}

static const char *xapi_findAttribute(const xapi_XmlNode *node, const char *name)
{
    if (!node || !name) { return NULL; }
    const xapi_XmlAttribute *attr = node->attributes;
    while (attr)
    {
        if (attr->name && strcmp(attr->name, name) == 0) { return attr->value; }
        attr = attr->next;
    }
    return NULL;
}

static bool xapi_parseOptionalDouble(const xapi_XmlNode *node, const char *name, double *outValue)
{
    const char *text = xapi_findAttribute(node, name);
    if (!text) { return false; }

    const char *cursor = text;
    double      parsed = 0.0;
    if (!xapi_parseDouble(&cursor, &parsed)) { return false; }
    *outValue = parsed;
    return true;
}

static int xapi_hexValue(char ch)
{
    if (ch >= '0' && ch <= '9') { return ch - '0'; }
    if (ch >= 'a' && ch <= 'f') { return ch - 'a' + 10; }
    if (ch >= 'A' && ch <= 'F') { return ch - 'A' + 10; }
    return -1;
}

static bool xapi_parseHexColor(const char *text, uint32_t *outColor)
{
    size_t len = strlen(text);
    if (len == 4 || len == 5)
    {
        int r = xapi_hexValue(text[1]);
        int g = xapi_hexValue(text[2]);
        int b = xapi_hexValue(text[3]);
        int a = (len == 5) ? xapi_hexValue(text[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) { return false; }
        *outColor = xapi_packColor((uint8_t)(r * 17), (uint8_t)(g * 17), (uint8_t)(b * 17),
                                                      (uint8_t)(a * 17));
        return true;
    }
    if (len == 7 || len == 9)
    {
        int values[8];
        for (size_t i = 1; i < len; i++)
        {
            values[i - 1] = xapi_hexValue(text[i]);
            if (values[i - 1] < 0) { return false; }
        }
        uint8_t r = (uint8_t)((values[0] << 4) | values[1]);
        uint8_t g = (uint8_t)((values[2] << 4) | values[3]);
        uint8_t b = (uint8_t)((values[4] << 4) | values[5]);
        uint8_t a = (len == 9) ? (uint8_t)((values[6] << 4) | values[7]) : 0xff;
        *outColor = xapi_packColor(r, g, b, a);
        return true;
    }
    return false;
}

static bool xapi_parseRgbColor(const char *text, uint32_t *outColor)
{
    if (!text || strncmp(text, "rgb(", 4) != 0) { return false; }
    const char *cursor = text + 4;
    double      rv = 0.0, gv = 0.0, bv = 0.0;
    if (!xapi_parseDouble(&cursor, &rv)) { return false; }
    if (!xapi_parseDouble(&cursor, &gv)) { return false; }
    if (!xapi_parseDouble(&cursor, &bv)) { return false; }
    xapi_skipDelimiters(&cursor);
    if (*cursor != ')') { return false; }

    int r = xapi_clampInt(xapi_roundToInt(rv), 0, 255);
    int g = xapi_clampInt(xapi_roundToInt(gv), 0, 255);
    int b = xapi_clampInt(xapi_roundToInt(bv), 0, 255);
    *outColor = xapi_packColor((uint8_t)r, (uint8_t)g, (uint8_t)b, 0xff);
    return true;
}

static bool xapi_parseNamedColor(const char *text, uint32_t *outColor)
{
    if (strcmp(text, "black") == 0) { *outColor = xapi_packColor(0, 0, 0, 0xff); return true; }
    if (strcmp(text, "white") == 0) { *outColor = xapi_packColor(255, 255, 255, 0xff); return true; }
    if (strcmp(text, "red") == 0) { *outColor = xapi_packColor(255, 0, 0, 0xff); return true; }
    if (strcmp(text, "green") == 0) { *outColor = xapi_packColor(0, 128, 0, 0xff); return true; }
    if (strcmp(text, "blue") == 0) { *outColor = xapi_packColor(0, 0, 255, 0xff); return true; }
    if (strcmp(text, "yellow") == 0) { *outColor = xapi_packColor(255, 255, 0, 0xff); return true; }
    if (strcmp(text, "gray") == 0 || strcmp(text, "grey") == 0)
    {
        *outColor = xapi_packColor(128, 128, 128, 0xff);
        return true;
    }
    if (strcmp(text, "cyan") == 0) { *outColor = xapi_packColor(0, 255, 255, 0xff); return true; }
    if (strcmp(text, "magenta") == 0) { *outColor = xapi_packColor(255, 0, 255, 0xff); return true; }
    if (strcmp(text, "orange") == 0) { *outColor = xapi_packColor(255, 165, 0, 0xff); return true; }
    return false;
}

static void xapi_trimSlice(const char *begin, const char *end, char *out, size_t outSize)
{
    while (begin < end && isspace((unsigned char)*begin))
    {
        begin++;
    }
    while (end > begin && isspace((unsigned char)*(end - 1)))
    {
        end--;
    }

    size_t len = (size_t)(end - begin);
    if (len >= outSize) { len = outSize - 1; }
    if (len > 0)
    {
        memcpy(out, begin, len);
    }
    out[len] = '\0';
}

static bool xapi_parseColorText(const char *text, uint32_t *outColor, bool *outEnabled)
{
    if (!text) { return false; }

    while (*text && isspace((unsigned char)*text))
    {
        text++;
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1]))
    {
        len--;
    }
    if (len == 0) { return false; }

    char buffer[64];
    if (len >= sizeof(buffer)) { len = sizeof(buffer) - 1; }
    memcpy(buffer, text, len);
    buffer[len] = '\0';

    if (strcmp(buffer, "none") == 0)
    {
        *outEnabled = false;
        return true;
    }

    if (strcmp(buffer, "transparent") == 0)
    {
        *outEnabled = true;
        *outColor   = xapi_packColor(0, 0, 0, 0x00);
        return true;
    }

    uint32_t color = 0;
    if (buffer[0] == '#' && xapi_parseHexColor(buffer, &color))
    {
        *outEnabled = true;
        *outColor   = color;
        return true;
    }

    if (xapi_parseRgbColor(buffer, &color) || xapi_parseNamedColor(buffer, &color))
    {
        *outEnabled = true;
        *outColor   = color;
        return true;
    }

    return false;
}

static void xapi_applyPaintToken(
    const char                   *key,
    const char                   *value,
    xapi_style *style)
{
    if (strcmp(key, "fill") == 0)
    {
        uint32_t color   = style->fillColor;
        bool     enabled = style->fillEnabled;
        if (xapi_parseColorText(value, &color, &enabled))
        {
            style->fillEnabled = enabled;
            style->fillColor   = color;
        }
        return;
    }

    if (strcmp(key, "stroke") == 0)
    {
        uint32_t color   = style->strokeColor;
        bool     enabled = style->strokeEnabled;
        if (xapi_parseColorText(value, &color, &enabled))
        {
            style->strokeEnabled = enabled;
            style->strokeColor   = color;
        }
        return;
    }

    if (strcmp(key, "stroke-width") == 0)
    {
        const char *cursor = value;
        double      width  = 0.0;
        if (xapi_parseDouble(&cursor, &width))
        {
            int stroke = xapi_roundToInt(width * 1.0);
            if (stroke < 1) { stroke = 1; }
            if (stroke > 64) { stroke = 64; }
            style->strokeWidth = stroke;
        }
        return;
    }

    if (strcmp(key, "fill-rule") == 0)
    {
        if (strcmp(value, "evenodd") == 0)
        {
            style->fillRuleEvenOdd = true;
        }
        else if (strcmp(value, "nonzero") == 0)
        {
            style->fillRuleEvenOdd = false;
        }
    }
}

static void xapi_applyInlineStyle(
    const char                   *styleText,
    xapi_style *style)
{
    if (!styleText) { return; }

    const char *cursor = styleText;
    while (*cursor)
    {
        const char *pairBegin = cursor;
        while (*cursor && *cursor != ';')
        {
            cursor++;
        }
        const char *pairEnd = cursor;
        if (*cursor == ';') { cursor++; }

        const char *colon = pairBegin;
        while (colon < pairEnd && *colon != ':')
        {
            colon++;
        }
        if (colon >= pairEnd) { continue; }

        char key[32];
        char value[64];
        xapi_trimSlice(pairBegin, colon, key, sizeof(key));
        xapi_trimSlice(colon + 1, pairEnd, value, sizeof(value));
        if (key[0] == '\0' || value[0] == '\0') { continue; }
        xapi_applyPaintToken(key, value, style);
    }
}

static void xapi_applyPresentationAttributes(
    const xapi_XmlNode            *node,
    xapi_style *style)
{
    const char *styleText = xapi_findAttribute(node, "style");
    if (styleText) { xapi_applyInlineStyle(styleText, style); }

    const char *fillAttr = xapi_findAttribute(node, "fill");
    if (fillAttr) { xapi_applyPaintToken("fill", fillAttr, style); }

    const char *strokeAttr = xapi_findAttribute(node, "stroke");
    if (strokeAttr) { xapi_applyPaintToken("stroke", strokeAttr, style); }

    const char *strokeWidthAttr = xapi_findAttribute(node, "stroke-width");
    if (strokeWidthAttr) { xapi_applyPaintToken("stroke-width", strokeWidthAttr, style); }

    const char *fillRuleAttr = xapi_findAttribute(node, "fill-rule");
    if (fillRuleAttr) { xapi_applyPaintToken("fill-rule", fillRuleAttr, style); }
}

static int xapi_integerSqrt(int value)
{
    if (value <= 0) { return 0; }

    int left = 0;
    int right = value < 46340 * 46340 ? value : 46340 * 46340;
    int ans = 0;
    while (left <= right)
    {
        int mid = left + (right - left) / 2;
        int64_t sq = (int64_t)mid * (int64_t)mid;
        if (sq <= value)
        {
            ans = mid;
            left = mid + 1;
        }
        else
        {
            right = mid - 1;
        }
    }
    return ans;
}

static void xapi_fillScreenRect(
    xapi_renderContext *ctx,
    int                                    x1,
    int                                    y1,
    int                                    x2,
    int                                    y2,
    uint32_t                               color)
{
    if (x1 > x2)
    {
        int tmp = x1;
        x1      = x2;
        x2      = tmp;
    }
    if (y1 > y2)
    {
        int tmp = y1;
        y1      = y2;
        y2      = tmp;
    }

    SHEET_BUFFER pixel = xapi_toSheetColor(xapi_applyTransColor(ctx, color));
    for (int y = y1; y <= y2; y++)
    {
        for (int x = x1; x <= x2; x++)
        {
            draw_point(ctx->sheetInfo, ctx->sheet, x, y, pixel);
        }
    }
}

static void xapi_renderRect(
    xapi_renderContext *ctx,
    const xapi_XmlNode                    *node,
    const xapi_style   *style)
{
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    xapi_parseOptionalDouble(node, "x", &x);
    xapi_parseOptionalDouble(node, "y", &y);
    if (!xapi_parseOptionalDouble(node, "width", &w)) { return; }
    if (!xapi_parseOptionalDouble(node, "height", &h)) { return; }
    if (w <= 0.0 || h <= 0.0) { return; }

    int sx1 = xapi_transformX(ctx, x);
    int sy1 = xapi_transformY(ctx, y);
    int sx2 = xapi_transformX(ctx, x + w);
    int sy2 = xapi_transformY(ctx, y + h);

    if (style->fillEnabled)
    {
        xapi_fillScreenRect(ctx, sx1, sy1, sx2, sy2, style->fillColor);
    }

    if (style->strokeEnabled)
    {
        int strokeWidth = xapi_clampInt(style->strokeWidth, 1, 64);
        xapi_drawScreenLineWithWidth(ctx, sx1, sy1, sx2, sy1, style->strokeColor, strokeWidth);
        xapi_drawScreenLineWithWidth(ctx, sx2, sy1, sx2, sy2, style->strokeColor, strokeWidth);
        xapi_drawScreenLineWithWidth(ctx, sx2, sy2, sx1, sy2, style->strokeColor, strokeWidth);
        xapi_drawScreenLineWithWidth(ctx, sx1, sy2, sx1, sy1, style->strokeColor, strokeWidth);
    }
}

static void xapi_renderCircle(
    xapi_renderContext *ctx,
    const xapi_XmlNode                    *node,
    const xapi_style   *style)
{
    double cx = 0.0, cy = 0.0, r = 0.0;
    if (!xapi_parseOptionalDouble(node, "cx", &cx)) { return; }
    if (!xapi_parseOptionalDouble(node, "cy", &cy)) { return; }
    if (!xapi_parseOptionalDouble(node, "r", &r)) { return; }
    if (r <= 0.0) { return; }

    int screenCx = xapi_transformX(ctx, cx);
    int screenCy = xapi_transformY(ctx, cy);
    int radius   = xapi_roundToInt(r * ctx->scale);
    if (radius < 1) { radius = 1; }

    if (style->fillEnabled)
    {
        SHEET_BUFFER pixel = xapi_toSheetColor(xapi_applyTransColor(ctx, style->fillColor));
        int          rr2   = radius * radius;
        for (int dy = -radius; dy <= radius; dy++)
        {
            int remain = rr2 - dy * dy;
            if (remain < 0) { continue; }
            int dx = xapi_integerSqrt(remain);
            for (int x = screenCx - dx; x <= screenCx + dx; x++)
            {
                draw_point(ctx->sheetInfo, ctx->sheet, x, screenCy + dy, pixel);
            }
        }
    }

    if (style->strokeEnabled)
    {
        int strokeWidth = xapi_clampInt(style->strokeWidth, 1, 64);
        int outer       = radius + strokeWidth / 2;
        int inner       = radius - (strokeWidth - 1) / 2;
        if (inner < 0) { inner = 0; }

        SHEET_BUFFER pixel = xapi_toSheetColor(xapi_applyTransColor(ctx, style->strokeColor));
        int          outer2 = outer * outer;
        int          inner2 = inner * inner;

        for (int dy = -outer; dy <= outer; dy++)
        {
            int remainOuter = outer2 - dy * dy;
            if (remainOuter < 0) { continue; }
            int dxOuter = xapi_integerSqrt(remainOuter);

            int dxInner = -1;
            if (inner > 0)
            {
                int remainInner = inner2 - dy * dy;
                if (remainInner >= 0)
                {
                    dxInner = xapi_integerSqrt(remainInner);
                }
            }

            for (int x = screenCx - dxOuter; x <= screenCx + dxOuter; x++)
            {
                if (dxInner >= 0 && x > screenCx - dxInner && x < screenCx + dxInner) { continue; }
                draw_point(ctx->sheetInfo, ctx->sheet, x, screenCy + dy, pixel);
            }
        }
    }
}

static bool xapi_parsePoints(
    const char *text,
    double    **outXs,
    double    **outYs,
    int        *outCount)
{
    if (!text || !outXs || !outYs || !outCount) { return false; }

    const char *cursor = text;
    int         count  = 0;
    while (true)
    {
        double x = 0.0;
        double y = 0.0;
        const char *backup = cursor;
        if (!xapi_parseDouble(&cursor, &x))
        {
            cursor = backup;
            break;
        }
        if (!xapi_parseDouble(&cursor, &y)) { break; }
        count++;
    }

    if (count < 2) { return false; }

    double *xs = (double *)malloc((size_t)count * sizeof(double));
    double *ys = (double *)malloc((size_t)count * sizeof(double));
    if (!xs || !ys)
    {
        free(xs);
        free(ys);
        return false;
    }

    cursor = text;
    for (int i = 0; i < count; i++)
    {
        if (!xapi_parseDouble(&cursor, &xs[i]) || !xapi_parseDouble(&cursor, &ys[i]))
        {
            free(xs);
            free(ys);
            return false;
        }
    }

    *outXs    = xs;
    *outYs    = ys;
    *outCount = count;
    return true;
}

static void xapi_fillPolygon(
    xapi_renderContext *ctx,
    const int                             *xs,
    const int                             *ys,
    int                                    count,
    uint32_t                               color)
{
    if (count < 3) { return; }

    int minY = ys[0];
    int maxY = ys[0];
    for (int i = 1; i < count; i++)
    {
        if (ys[i] < minY) { minY = ys[i]; }
        if (ys[i] > maxY) { maxY = ys[i]; }
    }

    int *intersections = (int *)malloc((size_t)count * sizeof(int));
    if (!intersections) { return; }

    SHEET_BUFFER pixel = xapi_toSheetColor(xapi_applyTransColor(ctx, color));

    for (int y = minY; y <= maxY; y++)
    {
        int hitCount = 0;
        for (int i = 0, j = count - 1; i < count; j = i++)
        {
            int y1 = ys[i];
            int y2 = ys[j];
            int x1 = xs[i];
            int x2 = xs[j];

            if (y1 == y2) { continue; }
            if ((y < MIN(y1, y2)) || (y >= MAX(y1, y2))) { continue; }

            double t = (double)(y - y1) / (double)(y2 - y1);
            intersections[hitCount++] = x1 + xapi_roundToInt((double)(x2 - x1) * t);
        }

        for (int i = 0; i < hitCount - 1; i++)
        {
            for (int j = i + 1; j < hitCount; j++)
            {
                if (intersections[i] > intersections[j])
                {
                    int tmp          = intersections[i];
                    intersections[i] = intersections[j];
                    intersections[j] = tmp;
                }
            }
        }

        for (int i = 0; i + 1 < hitCount; i += 2)
        {
            int xStart = intersections[i];
            int xEnd   = intersections[i + 1];
            for (int x = xStart; x <= xEnd; x++)
            {
                draw_point(ctx->sheetInfo, ctx->sheet, x, y, pixel);
            }
        }
    }

    free(intersections);
}

static void xapi_renderPolyline(
    xapi_renderContext *ctx,
    const xapi_XmlNode                    *node,
    const xapi_style   *style,
    bool                                   closed)
{
    const char *pointsText = xapi_findAttribute(node, "points");
    if (!pointsText) { return; }

    double *xs = NULL;
    double *ys = NULL;
    int     count = 0;
    if (!xapi_parsePoints(pointsText, &xs, &ys, &count)) { return; }

    int *screenXs = (int *)malloc((size_t)count * sizeof(int));
    int *screenYs = (int *)malloc((size_t)count * sizeof(int));
    if (!screenXs || !screenYs)
    {
        free(xs);
        free(ys);
        free(screenXs);
        free(screenYs);
        return;
    }

    for (int i = 0; i < count; i++)
    {
        screenXs[i] = xapi_transformX(ctx, xs[i]);
        screenYs[i] = xapi_transformY(ctx, ys[i]);
    }

    if (closed && style->fillEnabled)
    {
        xapi_fillPolygon(ctx, screenXs, screenYs, count, style->fillColor);
    }

    uint32_t lineColor = style->strokeColor;
    bool     drawLine  = style->strokeEnabled;
    int      lineWidth = xapi_clampInt(style->strokeWidth, 1, 64);

    if (!drawLine && style->fillEnabled)
    {
        drawLine  = true;
        lineColor = style->fillColor;
    }

    if (drawLine)
    {
        for (int i = 0; i + 1 < count; i++)
        {
            xapi_drawScreenLineWithWidth(
                ctx, screenXs[i], screenYs[i], screenXs[i + 1], screenYs[i + 1], lineColor, lineWidth);
        }
        if (closed)
        {
            xapi_drawScreenLineWithWidth(
                ctx, screenXs[count - 1], screenYs[count - 1], screenXs[0], screenYs[0], lineColor, lineWidth);
        }
    }

    free(xs);
    free(ys);
    free(screenXs);
    free(screenYs);
}

static void xapi_renderQuadraticCurve(
    xapi_renderContext *ctx,
    double                                 x0,
    double                                 y0,
    double                                 x1,
    double                                 y1,
    double                                 x2,
    double                                 y2,
    uint32_t                               color,
    int                                    lineWidth)
{
    double sx0 = xapi_transformXf(ctx, x0);
    double sy0 = xapi_transformYf(ctx, y0);
    double sx1 = xapi_transformXf(ctx, x1);
    double sy1 = xapi_transformYf(ctx, y1);
    double sx2 = xapi_transformXf(ctx, x2);
    double sy2 = xapi_transformYf(ctx, y2);

    double estimate = ABS(sx1 - sx0) + ABS(sy1 - sy0) + ABS(sx2 - sx1) + ABS(sy2 - sy1);
    int segments    = xapi_clampInt((int)(estimate / 2.0), 16, 2048);

    double prevX = sx0;
    double prevY = sy0;
    for (int i = 1; i <= segments; i++)
    {
        double t  = (double)i / (double)segments;
        double mt = 1.0 - t;
        double x  = mt * mt * sx0 + 2.0 * mt * t * sx1 + t * t * sx2;
        double y  = mt * mt * sy0 + 2.0 * mt * t * sy1 + t * t * sy2;
        xapi_drawScreenLineWithWidth(
            ctx,
            xapi_roundToInt(prevX),
            xapi_roundToInt(prevY),
            xapi_roundToInt(x),
            xapi_roundToInt(y),
            color,
            lineWidth);
        prevX = x;
        prevY = y;
    }
}

static void xapi_renderCubicCurve(
    xapi_renderContext *ctx,
    double                                 x0,
    double                                 y0,
    double                                 x1,
    double                                 y1,
    double                                 x2,
    double                                 y2,
    double                                 x3,
    double                                 y3,
    uint32_t                               color,
    int                                    lineWidth)
{
    double sx0 = xapi_transformXf(ctx, x0);
    double sy0 = xapi_transformYf(ctx, y0);
    double sx1 = xapi_transformXf(ctx, x1);
    double sy1 = xapi_transformYf(ctx, y1);
    double sx2 = xapi_transformXf(ctx, x2);
    double sy2 = xapi_transformYf(ctx, y2);
    double sx3 = xapi_transformXf(ctx, x3);
    double sy3 = xapi_transformYf(ctx, y3);

    double estimate =
        ABS(sx1 - sx0) + ABS(sy1 - sy0) + ABS(sx2 - sx1) + ABS(sy2 - sy1) + ABS(sx3 - sx2) + ABS(sy3 - sy2);
    int segments = xapi_clampInt((int)(estimate / 2.0), 20, 2048);

    double prevX = sx0;
    double prevY = sy0;
    for (int i = 1; i <= segments; i++)
    {
        double t  = (double)i / (double)segments;
        double mt = 1.0 - t;

        double x = mt * mt * mt * sx0 + 3.0 * mt * mt * t * sx1 + 3.0 * mt * t * t * sx2 + t * t * t * sx3;
        double y = mt * mt * mt * sy0 + 3.0 * mt * mt * t * sy1 + 3.0 * mt * t * t * sy2 + t * t * t * sy3;

        xapi_drawScreenLineWithWidth(
            ctx,
            xapi_roundToInt(prevX),
            xapi_roundToInt(prevY),
            xapi_roundToInt(x),
            xapi_roundToInt(y),
            color,
            lineWidth);
        prevX = x;
        prevY = y;
    }
}

static bool xapi_isPathCommand(char ch)
{
    switch (ch)
    {
    case 'M':
    case 'm':
    case 'L':
    case 'l':
    case 'H':
    case 'h':
    case 'V':
    case 'v':
    case 'C':
    case 'c':
    case 'S':
    case 's':
    case 'Q':
    case 'q':
    case 'T':
    case 't':
    case 'A':
    case 'a':
    case 'Z':
    case 'z': return true;
    default: return false;
    }
}

static bool xapi_pathPointListReserve(xapi_pathPointList *list, int required)
{
    if (!list) { return false; }
    if (required <= list->capacity) { return true; }

    int newCapacity = list->capacity > 0 ? list->capacity : 128;
    while (newCapacity < required)
    {
        if (newCapacity > (1 << 20)) { return false; }
        newCapacity *= 2;
    }

    int *newXs = (int *)malloc((size_t)newCapacity * sizeof(int));
    int *newYs = (int *)malloc((size_t)newCapacity * sizeof(int));
    if (!newXs || !newYs)
    {
        free(newXs);
        free(newYs);
        return false;
    }

    if (list->count > 0)
    {
        memcpy(newXs, list->xs, (size_t)list->count * sizeof(int));
        memcpy(newYs, list->ys, (size_t)list->count * sizeof(int));
    }

    free(list->xs);
    free(list->ys);
    list->xs       = newXs;
    list->ys       = newYs;
    list->capacity = newCapacity;
    return true;
}

static bool xapi_pathPointListAppend(xapi_pathPointList *list, int x, int y)
{
    if (!list) { return false; }
    if (list->count > 0 && list->xs[list->count - 1] == x && list->ys[list->count - 1] == y) { return true; }
    if (!xapi_pathPointListReserve(list, list->count + 1)) { return false; }
    list->xs[list->count] = x;
    list->ys[list->count] = y;
    list->count++;
    return true;
}

static void xapi_pathPointListReset(xapi_pathPointList *list)
{
    if (!list) { return; }
    list->count = 0;
}

static void xapi_pathPointListFree(xapi_pathPointList *list)
{
    if (!list) { return; }
    free(list->xs);
    free(list->ys);
    list->xs       = NULL;
    list->ys       = NULL;
    list->count    = 0;
    list->capacity = 0;
}

static bool xapi_fillContourListReserve(xapi_fillContourList *list, int required)
{
    if (!list) { return false; }
    if (required <= list->capacity) { return true; }

    int newCapacity = list->capacity > 0 ? list->capacity : 8;
    while (newCapacity < required)
    {
        if (newCapacity > (1 << 20)) { return false; }
        newCapacity *= 2;
    }

    xapi_fillContour *newItems = (xapi_fillContour *)malloc((size_t)newCapacity * sizeof(xapi_fillContour));
    if (!newItems) { return false; }

    if (list->count > 0)
    {
        memcpy(newItems, list->items, (size_t)list->count * sizeof(xapi_fillContour));
    }
    free(list->items);
    list->items    = newItems;
    list->capacity = newCapacity;
    return true;
}

static bool xapi_fillContourListAppendFromPath(xapi_fillContourList *list, const xapi_pathPointList *pathPoints)
{
    if (!list || !pathPoints || pathPoints->count < 3) { return true; }
    if (!xapi_fillContourListReserve(list, list->count + 1)) { return false; }

    xapi_fillContour contour;
    contour.count = pathPoints->count;
    contour.xs    = (int *)malloc((size_t)contour.count * sizeof(int));
    contour.ys    = (int *)malloc((size_t)contour.count * sizeof(int));
    if (!contour.xs || !contour.ys)
    {
        free(contour.xs);
        free(contour.ys);
        return false;
    }

    memcpy(contour.xs, pathPoints->xs, (size_t)contour.count * sizeof(int));
    memcpy(contour.ys, pathPoints->ys, (size_t)contour.count * sizeof(int));
    list->items[list->count] = contour;
    list->count++;
    return true;
}

static void xapi_fillContourListFree(xapi_fillContourList *list)
{
    if (!list) { return; }
    for (int i = 0; i < list->count; i++)
    {
        free(list->items[i].xs);
        free(list->items[i].ys);
        list->items[i].xs    = NULL;
        list->items[i].ys    = NULL;
        list->items[i].count = 0;
    }
    free(list->items);
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

static void xapi_fillContours(
    xapi_renderContext        *ctx,
    const xapi_fillContourList *contours,
    uint32_t                   color,
    bool                       useEvenOddRule)
{
    if (!ctx || !contours || contours->count <= 0) { return; }

    int minY = 0;
    int maxY = 0;
    bool hasPoint = false;
    int totalEdges = 0;
    for (int c = 0; c < contours->count; c++)
    {
        const xapi_fillContour *contour = &contours->items[c];
        if (!contour->xs || !contour->ys || contour->count < 3) { continue; }
        totalEdges += contour->count;
        for (int i = 0; i < contour->count; i++)
        {
            int y = contour->ys[i];
            if (!hasPoint)
            {
                minY     = y;
                maxY     = y;
                hasPoint = true;
            }
            else
            {
                if (y < minY) { minY = y; }
                if (y > maxY) { maxY = y; }
            }
        }
    }
    if (!hasPoint || totalEdges <= 0) { return; }

    xapi_scanIntersection *intersections =
        (xapi_scanIntersection *)malloc((size_t)totalEdges * sizeof(xapi_scanIntersection));
    if (!intersections) { return; }

    SHEET_BUFFER pixel = xapi_toSheetColor(xapi_applyTransColor(ctx, color));
    for (int y = minY; y <= maxY; y++)
    {
        int hitCount = 0;
        for (int c = 0; c < contours->count; c++)
        {
            const xapi_fillContour *contour = &contours->items[c];
            if (!contour->xs || !contour->ys || contour->count < 3) { continue; }

            for (int i = 0, j = contour->count - 1; i < contour->count; j = i++)
            {
                int x1 = contour->xs[j];
                int y1 = contour->ys[j];
                int x2 = contour->xs[i];
                int y2 = contour->ys[i];

                if (y1 == y2) { continue; }
                if (y < MIN(y1, y2) || y >= MAX(y1, y2)) { continue; }

                double t = (double)(y - y1) / (double)(y2 - y1);
                intersections[hitCount].x            = x1 + xapi_roundToInt((double)(x2 - x1) * t);
                intersections[hitCount].windingDelta = (y2 > y1) ? 1 : -1;
                hitCount++;
            }
        }

        for (int i = 0; i < hitCount - 1; i++)
        {
            for (int j = i + 1; j < hitCount; j++)
            {
                if (intersections[i].x > intersections[j].x)
                {
                    xapi_scanIntersection tmp = intersections[i];
                    intersections[i]          = intersections[j];
                    intersections[j]          = tmp;
                }
            }
        }

        if (useEvenOddRule)
        {
            for (int i = 0; i + 1 < hitCount; i += 2)
            {
                int xStart = intersections[i].x;
                int xEnd   = intersections[i + 1].x;
                for (int x = xStart; x <= xEnd; x++)
                {
                    draw_point(ctx->sheetInfo, ctx->sheet, x, y, pixel);
                }
            }
            continue;
        }

        int  winding = 0;
        bool hasPrev = false;
        int  prevX   = 0;
        for (int i = 0; i < hitCount; i++)
        {
            int x = intersections[i].x;
            if (hasPrev && winding != 0)
            {
                for (int fillX = prevX; fillX <= x; fillX++)
                {
                    draw_point(ctx->sheetInfo, ctx->sheet, fillX, y, pixel);
                }
            }
            winding += intersections[i].windingDelta;
            prevX    = x;
            hasPrev  = true;
        }
    }

    free(intersections);
}

static bool xapi_appendQuadraticCurvePoints(
    xapi_renderContext *ctx,
    xapi_pathPointList *points,
    double              x0,
    double              y0,
    double              x1,
    double              y1,
    double              x2,
    double              y2)
{
    double sx0 = xapi_transformXf(ctx, x0);
    double sy0 = xapi_transformYf(ctx, y0);
    double sx1 = xapi_transformXf(ctx, x1);
    double sy1 = xapi_transformYf(ctx, y1);
    double sx2 = xapi_transformXf(ctx, x2);
    double sy2 = xapi_transformYf(ctx, y2);

    double estimate = ABS(sx1 - sx0) + ABS(sy1 - sy0) + ABS(sx2 - sx1) + ABS(sy2 - sy1);
    int segments    = xapi_clampInt((int)(estimate / 2.0), 16, 2048);

    for (int i = 1; i <= segments; i++)
    {
        double t  = (double)i / (double)segments;
        double mt = 1.0 - t;
        double x  = mt * mt * sx0 + 2.0 * mt * t * sx1 + t * t * sx2;
        double y  = mt * mt * sy0 + 2.0 * mt * t * sy1 + t * t * sy2;
        if (!xapi_pathPointListAppend(points, xapi_roundToInt(x), xapi_roundToInt(y))) { return false; }
    }
    return true;
}

static bool xapi_appendCubicCurvePoints(
    xapi_renderContext *ctx,
    xapi_pathPointList *points,
    double              x0,
    double              y0,
    double              x1,
    double              y1,
    double              x2,
    double              y2,
    double              x3,
    double              y3)
{
    double sx0 = xapi_transformXf(ctx, x0);
    double sy0 = xapi_transformYf(ctx, y0);
    double sx1 = xapi_transformXf(ctx, x1);
    double sy1 = xapi_transformYf(ctx, y1);
    double sx2 = xapi_transformXf(ctx, x2);
    double sy2 = xapi_transformYf(ctx, y2);
    double sx3 = xapi_transformXf(ctx, x3);
    double sy3 = xapi_transformYf(ctx, y3);

    double estimate =
        ABS(sx1 - sx0) + ABS(sy1 - sy0) + ABS(sx2 - sx1) + ABS(sy2 - sy1) + ABS(sx3 - sx2) + ABS(sy3 - sy2);
    int segments = xapi_clampInt((int)(estimate / 2.0), 20, 2048);

    for (int i = 1; i <= segments; i++)
    {
        double t  = (double)i / (double)segments;
        double mt = 1.0 - t;

        double x = mt * mt * mt * sx0 + 3.0 * mt * mt * t * sx1 + 3.0 * mt * t * t * sx2 + t * t * t * sx3;
        double y = mt * mt * mt * sy0 + 3.0 * mt * mt * t * sy1 + 3.0 * mt * t * t * sy2 + t * t * t * sy3;
        if (!xapi_pathPointListAppend(points, xapi_roundToInt(x), xapi_roundToInt(y))) { return false; }
    }
    return true;
}

static bool xapi_renderArcToPath(
    xapi_renderContext *ctx,
    xapi_pathPointList *subPathPoints,
    bool                fillOn,
    bool                strokeOn,
    uint32_t            strokeColor,
    int                 lineWidth,
    double              startX,
    double              startY,
    double              radiusX,
    double              radiusY,
    double              xAxisRotation,
    bool                largeArcFlag,
    bool                sweepFlag,
    double              endX,
    double              endY)
{
    const double pi     = 3.14159265358979323846;
    const double twoPi  = 6.28318530717958647692;
    const double epsilon = 1e-9;

    auto appendEndpoint = [&]() -> bool {
        if (!fillOn) { return true; }
        return xapi_pathPointListAppend(
            subPathPoints, xapi_transformX(ctx, endX), xapi_transformY(ctx, endY));
    };

    if ((xapi_absDouble(endX - startX) < epsilon && xapi_absDouble(endY - startY) < epsilon) || radiusX == 0.0 ||
        radiusY == 0.0)
    {
        if (strokeOn)
        {
            xapi_drawSvgLine(
                ctx, startX, startY, endX, endY, strokeColor, lineWidth);
        }
        return appendEndpoint();
    }

    double rx = xapi_absDouble(radiusX);
    double ry = xapi_absDouble(radiusY);
    if (rx < epsilon || ry < epsilon)
    {
        if (strokeOn)
        {
            xapi_drawSvgLine(
                ctx, startX, startY, endX, endY, strokeColor, lineWidth);
        }
        return appendEndpoint();
    }

    double phi    = xAxisRotation * (pi / 180.0);
    double cosPhi = xapi_cosApprox(phi);
    double sinPhi = xapi_sinApprox(phi);

    double dx2 = (startX - endX) * 0.5;
    double dy2 = (startY - endY) * 0.5;

    double x1p = cosPhi * dx2 + sinPhi * dy2;
    double y1p = -sinPhi * dx2 + cosPhi * dy2;

    double rx2 = rx * rx;
    double ry2 = ry * ry;
    double x1p2 = x1p * x1p;
    double y1p2 = y1p * y1p;

    double lambda = x1p2 / rx2 + y1p2 / ry2;
    if (lambda > 1.0)
    {
        double scale = xapi_sqrtDouble(lambda);
        rx *= scale;
        ry *= scale;
        rx2 = rx * rx;
        ry2 = ry * ry;
    }

    double denominator = rx2 * y1p2 + ry2 * x1p2;
    if (denominator <= epsilon)
    {
        if (strokeOn)
        {
            xapi_drawSvgLine(
                ctx, startX, startY, endX, endY, strokeColor, lineWidth);
        }
        return appendEndpoint();
    }

    double numerator = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
    if (numerator < 0.0) { numerator = 0.0; }

    double sign = (largeArcFlag == sweepFlag) ? -1.0 : 1.0;
    double coef = sign * xapi_sqrtDouble(numerator / denominator);

    double cxp = coef * ((rx * y1p) / ry);
    double cyp = coef * (-(ry * x1p) / rx);

    double cx = cosPhi * cxp - sinPhi * cyp + (startX + endX) * 0.5;
    double cy = sinPhi * cxp + cosPhi * cyp + (startY + endY) * 0.5;

    double ux = (x1p - cxp) / rx;
    double uy = (y1p - cyp) / ry;
    double vx = (-x1p - cxp) / rx;
    double vy = (-y1p - cyp) / ry;

    double startAngle = xapi_atan2Approx(uy, ux);
    double deltaAngle = xapi_atan2Approx(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweepFlag && deltaAngle > 0.0)
    {
        deltaAngle -= twoPi;
    }
    else if (sweepFlag && deltaAngle < 0.0)
    {
        deltaAngle += twoPi;
    }

    double radiusScreen = (rx > ry ? rx : ry) * ctx->scale;
    int    segmentsByAngle =
        xapi_clampInt((int)(xapi_absDouble(deltaAngle) / (pi / 24.0)) + 1, 1, 2048);
    int segmentsByRadius = xapi_clampInt((int)(xapi_absDouble(deltaAngle) * radiusScreen / 3.0) + 1, 1, 2048);
    int segments         = segmentsByAngle > segmentsByRadius ? segmentsByAngle : segmentsByRadius;

    double prevX = startX;
    double prevY = startY;
    for (int i = 1; i <= segments; i++)
    {
        double nextX = endX;
        double nextY = endY;
        if (i != segments)
        {
            double t       = (double)i / (double)segments;
            double angle   = startAngle + deltaAngle * t;
            double cosA    = xapi_cosApprox(angle);
            double sinA    = xapi_sinApprox(angle);
            nextX          = cx + cosPhi * rx * cosA - sinPhi * ry * sinA;
            nextY          = cy + sinPhi * rx * cosA + cosPhi * ry * sinA;
        }

        if (strokeOn)
        {
            xapi_drawSvgLine(
                ctx, prevX, prevY, nextX, nextY, strokeColor, lineWidth);
        }

        if (fillOn &&
            !xapi_pathPointListAppend(subPathPoints, xapi_transformX(ctx, nextX), xapi_transformY(ctx, nextY)))
        {
            return false;
        }

        prevX = nextX;
        prevY = nextY;
    }

    return true;
}

static void xapi_renderPath(
    xapi_renderContext *ctx,
    const xapi_XmlNode                    *node,
    const xapi_style   *style)
{
    const char *d = xapi_findAttribute(node, "d");
    if (!d) { return; }

    uint32_t fillColor   = style->fillColor;
    bool     fillOn      = style->fillEnabled;
    uint32_t strokeColor = style->strokeColor;
    bool     strokeOn    = style->strokeEnabled;
    int      lineWidth   = xapi_clampInt(style->strokeWidth, 1, 64);
    if (!fillOn && !strokeOn) { return; }

    const char *cursor = d;
    char        command = 0;
    char        previous = 0;

    double currentX = 0.0;
    double currentY = 0.0;
    double subPathX = 0.0;
    double subPathY = 0.0;
    double lastCubicCtrlX = 0.0;
    double lastCubicCtrlY = 0.0;
    double lastQuadCtrlX  = 0.0;
    double lastQuadCtrlY  = 0.0;
    bool   hasSubPath     = false;
    bool   parseFailed    = false;

    xapi_pathPointList subPathPoints;
    memset(&subPathPoints, 0, sizeof(subPathPoints));
    xapi_fillContourList fillContours;
    memset(&fillContours, 0, sizeof(fillContours));

    while (true)
    {
        xapi_skipDelimiters(&cursor);
        if (*cursor == '\0') { break; }

        if (xapi_isPathCommand(*cursor))
        {
            command = *cursor;
            cursor++;
        }
        else if (command == 0)
        {
            break;
        }

        bool relative = (command >= 'a' && command <= 'z');
        char upper    = (command >= 'a' && command <= 'z') ? (char)(command - ('a' - 'A')) : command;

        if (upper == 'Z')
        {
            if (!hasSubPath)
            {
                previous = command;
                continue;
            }
            if (strokeOn)
            {
                xapi_drawSvgLine(
                    ctx, currentX, currentY, subPathX, subPathY, strokeColor, lineWidth);
            }
            if (fillOn)
            {
                if (!xapi_pathPointListAppend(
                        &subPathPoints, xapi_transformX(ctx, subPathX), xapi_transformY(ctx, subPathY)))
                {
                    parseFailed = true;
                    break;
                }
                if (!xapi_fillContourListAppendFromPath(&fillContours, &subPathPoints))
                {
                    parseFailed = true;
                    break;
                }
                xapi_pathPointListReset(&subPathPoints);
            }
            currentX = subPathX;
            currentY = subPathY;
            hasSubPath = false;
            previous = command;
            continue;
        }

        if (upper == 'M')
        {
            double x = 0.0, y = 0.0;
            if (!xapi_parseDouble(&cursor, &x) || !xapi_parseDouble(&cursor, &y))
            {
                break;
            }

            if (fillOn && hasSubPath && subPathPoints.count >= 3 &&
                !xapi_fillContourListAppendFromPath(&fillContours, &subPathPoints))
            {
                parseFailed = true;
                break;
            }
            xapi_pathPointListReset(&subPathPoints);

            if (relative)
            {
                currentX += x;
                currentY += y;
            }
            else
            {
                currentX = x;
                currentY = y;
            }
            subPathX = currentX;
            subPathY = currentY;
            hasSubPath = true;
            if (fillOn)
            {
                if (!xapi_pathPointListAppend(
                        &subPathPoints, xapi_transformX(ctx, currentX), xapi_transformY(ctx, currentY)))
                {
                    parseFailed = true;
                    break;
                }
            }
            previous = command;

            while (true)
            {
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &x) || !xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }
                double nextX = relative ? (currentX + x) : x;
                double nextY = relative ? (currentY + y) : y;
                if (strokeOn)
                {
                    xapi_drawSvgLine(
                        ctx, currentX, currentY, nextX, nextY, strokeColor, lineWidth);
                }
                if (fillOn)
                {
                    if (!xapi_pathPointListAppend(
                            &subPathPoints, xapi_transformX(ctx, nextX), xapi_transformY(ctx, nextY)))
                    {
                        parseFailed = true;
                        break;
                    }
                }
                currentX = nextX;
                currentY = nextY;
                previous = relative ? 'l' : 'L';
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'L')
        {
            while (true)
            {
                double x = 0.0, y = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &x) || !xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }
                double nextX = relative ? (currentX + x) : x;
                double nextY = relative ? (currentY + y) : y;
                if (strokeOn)
                {
                    xapi_drawSvgLine(
                        ctx, currentX, currentY, nextX, nextY, strokeColor, lineWidth);
                }
                if (fillOn)
                {
                    if (!xapi_pathPointListAppend(
                            &subPathPoints, xapi_transformX(ctx, nextX), xapi_transformY(ctx, nextY)))
                    {
                        parseFailed = true;
                        break;
                    }
                }
                currentX = nextX;
                currentY = nextY;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'H')
        {
            while (true)
            {
                double x = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &x))
                {
                    cursor = backup;
                    break;
                }
                double nextX = relative ? (currentX + x) : x;
                if (strokeOn)
                {
                    xapi_drawSvgLine(
                        ctx, currentX, currentY, nextX, currentY, strokeColor, lineWidth);
                }
                if (fillOn)
                {
                    if (!xapi_pathPointListAppend(
                            &subPathPoints, xapi_transformX(ctx, nextX), xapi_transformY(ctx, currentY)))
                    {
                        parseFailed = true;
                        break;
                    }
                }
                currentX = nextX;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'V')
        {
            while (true)
            {
                double y = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }
                double nextY = relative ? (currentY + y) : y;
                if (strokeOn)
                {
                    xapi_drawSvgLine(
                        ctx, currentX, currentY, currentX, nextY, strokeColor, lineWidth);
                }
                if (fillOn)
                {
                    if (!xapi_pathPointListAppend(
                            &subPathPoints, xapi_transformX(ctx, currentX), xapi_transformY(ctx, nextY)))
                    {
                        parseFailed = true;
                        break;
                    }
                }
                currentY = nextY;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'C')
        {
            while (true)
            {
                double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &x1) || !xapi_parseDouble(&cursor, &y1) ||
                    !xapi_parseDouble(&cursor, &x2) || !xapi_parseDouble(&cursor, &y2) ||
                    !xapi_parseDouble(&cursor, &x) || !xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }

                double c1x = relative ? (currentX + x1) : x1;
                double c1y = relative ? (currentY + y1) : y1;
                double c2x = relative ? (currentX + x2) : x2;
                double c2y = relative ? (currentY + y2) : y2;
                double ex  = relative ? (currentX + x) : x;
                double ey  = relative ? (currentY + y) : y;

                if (strokeOn)
                {
                    xapi_renderCubicCurve(
                        ctx, currentX, currentY, c1x, c1y, c2x, c2y, ex, ey, strokeColor, lineWidth);
                }
                if (fillOn && !xapi_appendCubicCurvePoints(
                                  ctx, &subPathPoints, currentX, currentY, c1x, c1y, c2x, c2y, ex, ey))
                {
                    parseFailed = true;
                    break;
                }

                currentX = ex;
                currentY = ey;
                lastCubicCtrlX = c2x;
                lastCubicCtrlY = c2y;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'S')
        {
            while (true)
            {
                double x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &x2) || !xapi_parseDouble(&cursor, &y2) ||
                    !xapi_parseDouble(&cursor, &x) || !xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }

                bool wasCubic = (previous == 'C' || previous == 'c' || previous == 'S' || previous == 's');
                double c1x    = wasCubic ? (2.0 * currentX - lastCubicCtrlX) : currentX;
                double c1y    = wasCubic ? (2.0 * currentY - lastCubicCtrlY) : currentY;
                double c2x    = relative ? (currentX + x2) : x2;
                double c2y    = relative ? (currentY + y2) : y2;
                double ex     = relative ? (currentX + x) : x;
                double ey     = relative ? (currentY + y) : y;

                if (strokeOn)
                {
                    xapi_renderCubicCurve(
                        ctx, currentX, currentY, c1x, c1y, c2x, c2y, ex, ey, strokeColor, lineWidth);
                }
                if (fillOn && !xapi_appendCubicCurvePoints(
                                  ctx, &subPathPoints, currentX, currentY, c1x, c1y, c2x, c2y, ex, ey))
                {
                    parseFailed = true;
                    break;
                }

                currentX = ex;
                currentY = ey;
                lastCubicCtrlX = c2x;
                lastCubicCtrlY = c2y;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'Q')
        {
            while (true)
            {
                double x1 = 0.0, y1 = 0.0, x = 0.0, y = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &x1) || !xapi_parseDouble(&cursor, &y1) ||
                    !xapi_parseDouble(&cursor, &x) || !xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }

                double c1x = relative ? (currentX + x1) : x1;
                double c1y = relative ? (currentY + y1) : y1;
                double ex  = relative ? (currentX + x) : x;
                double ey  = relative ? (currentY + y) : y;

                if (strokeOn)
                {
                    xapi_renderQuadraticCurve(
                        ctx, currentX, currentY, c1x, c1y, ex, ey, strokeColor, lineWidth);
                }
                if (fillOn && !xapi_appendQuadraticCurvePoints(
                                  ctx, &subPathPoints, currentX, currentY, c1x, c1y, ex, ey))
                {
                    parseFailed = true;
                    break;
                }

                currentX = ex;
                currentY = ey;
                lastQuadCtrlX = c1x;
                lastQuadCtrlY = c1y;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'T')
        {
            while (true)
            {
                double x = 0.0, y = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &x) || !xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }

                bool wasQuad = (previous == 'Q' || previous == 'q' || previous == 'T' || previous == 't');
                double c1x   = wasQuad ? (2.0 * currentX - lastQuadCtrlX) : currentX;
                double c1y   = wasQuad ? (2.0 * currentY - lastQuadCtrlY) : currentY;
                double ex    = relative ? (currentX + x) : x;
                double ey    = relative ? (currentY + y) : y;

                if (strokeOn)
                {
                    xapi_renderQuadraticCurve(
                        ctx, currentX, currentY, c1x, c1y, ex, ey, strokeColor, lineWidth);
                }
                if (fillOn && !xapi_appendQuadraticCurvePoints(
                                  ctx, &subPathPoints, currentX, currentY, c1x, c1y, ex, ey))
                {
                    parseFailed = true;
                    break;
                }

                currentX = ex;
                currentY = ey;
                lastQuadCtrlX = c1x;
                lastQuadCtrlY = c1y;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        if (upper == 'A')
        {
            while (true)
            {
                double rx = 0.0, ry = 0.0, axis = 0.0, largeArc = 0.0, sweep = 0.0, x = 0.0, y = 0.0;
                const char *backup = cursor;
                if (!xapi_parseDouble(&cursor, &rx) || !xapi_parseDouble(&cursor, &ry) ||
                    !xapi_parseDouble(&cursor, &axis) || !xapi_parseDouble(&cursor, &largeArc) ||
                    !xapi_parseDouble(&cursor, &sweep) || !xapi_parseDouble(&cursor, &x) ||
                    !xapi_parseDouble(&cursor, &y))
                {
                    cursor = backup;
                    break;
                }

                double ex = relative ? (currentX + x) : x;
                double ey = relative ? (currentY + y) : y;
                bool largeArcFlag = largeArc >= 0.5;
                bool sweepFlag    = sweep >= 0.5;
                if (!xapi_renderArcToPath(
                        ctx,
                        &subPathPoints,
                        fillOn,
                        strokeOn,
                        strokeColor,
                        lineWidth,
                        currentX,
                        currentY,
                        rx,
                        ry,
                        axis,
                        largeArcFlag,
                        sweepFlag,
                        ex,
                        ey))
                {
                    parseFailed = true;
                    break;
                }
                currentX = ex;
                currentY = ey;
                previous = command;
            }
            if (parseFailed) { break; }
            continue;
        }

        break;
    }

    if (fillOn && !parseFailed && hasSubPath && subPathPoints.count >= 3 &&
        !xapi_fillContourListAppendFromPath(&fillContours, &subPathPoints))
    {
        parseFailed = true;
    }

    if (fillOn && !parseFailed && fillContours.count > 0)
    {
        xapi_fillContours(ctx, &fillContours, fillColor, style->fillRuleEvenOdd);
    }
    xapi_fillContourListFree(&fillContours);
    xapi_pathPointListFree(&subPathPoints);
}

static void xapi_renderLineElement(
    xapi_renderContext *ctx,
    const xapi_XmlNode                    *node,
    const xapi_style   *style)
{
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    if (!xapi_parseOptionalDouble(node, "x1", &x1)) { return; }
    if (!xapi_parseOptionalDouble(node, "y1", &y1)) { return; }
    if (!xapi_parseOptionalDouble(node, "x2", &x2)) { return; }
    if (!xapi_parseOptionalDouble(node, "y2", &y2)) { return; }

    uint32_t lineColor = style->strokeColor;
    bool     drawLine  = style->strokeEnabled;
    int      lineWidth = xapi_clampInt(style->strokeWidth, 1, 64);

    if (!drawLine && style->fillEnabled)
    {
        drawLine  = true;
        lineColor = style->fillColor;
    }

    if (drawLine)
    {
        xapi_drawSvgLine(ctx, x1, y1, x2, y2, lineColor, lineWidth);
    }
}

static void xapi_renderNode(
    xapi_renderContext *ctx,
    const xapi_XmlNode                    *node,
    const xapi_style   *parentStyle)
{
    if (!node || node->type != XAPI_XML_NODE_ELEMENT || !node->name) { return; }

    xapi_style style = *parentStyle;
    xapi_applyPresentationAttributes(node, &style);

    if (strcmp(node->name, "rect") == 0)
    {
        xapi_renderRect(ctx, node, &style);
    }
    else if (strcmp(node->name, "circle") == 0)
    {
        xapi_renderCircle(ctx, node, &style);
    }
    else if (strcmp(node->name, "line") == 0)
    {
        xapi_renderLineElement(ctx, node, &style);
    }
    else if (strcmp(node->name, "polyline") == 0)
    {
        xapi_renderPolyline(ctx, node, &style, false);
    }
    else if (strcmp(node->name, "polygon") == 0)
    {
        xapi_renderPolyline(ctx, node, &style, true);
    }
    else if (strcmp(node->name, "path") == 0)
    {
        xapi_renderPath(ctx, node, &style);
    }

    const xapi_XmlNode *child = node->firstChild;
    while (child)
    {
        xapi_renderNode(ctx, child, &style);
        child = child->nextSibling;
    }
}

static bool xapi_parseViewBox(
    const xapi_XmlNode *svgNode,
    double             *outMinX,
    double             *outMinY,
    double             *outWidth,
    double             *outHeight)
{
    const char *viewBoxText = xapi_findAttribute(svgNode, "viewBox");
    if (!viewBoxText) { return false; }

    const char *cursor = viewBoxText;
    double      minX   = 0.0;
    double      minY   = 0.0;
    double      width  = 0.0;
    double      height = 0.0;

    if (!xapi_parseDouble(&cursor, &minX) || !xapi_parseDouble(&cursor, &minY) ||
        !xapi_parseDouble(&cursor, &width) || !xapi_parseDouble(&cursor, &height))
    {
        return false;
    }

    if (width <= 0.0 || height <= 0.0) { return false; }

    *outMinX  = minX;
    *outMinY  = minY;
    *outWidth = width;
    *outHeight = height;
    return true;
}

static bool xapi_resolveViewMetrics(
    const xapi_XmlNode *svgNode,
    int                 targetWidth,
    double             *outMinX,
    double             *outMinY,
    double             *outViewWidth,
    double             *outViewHeight,
    int                *outTargetHeight)
{
    double minX = 0.0;
    double minY = 0.0;
    double viewWidth = 0.0;
    double viewHeight = 0.0;

    bool hasViewBox = xapi_parseViewBox(svgNode, &minX, &minY, &viewWidth, &viewHeight);
    if (!hasViewBox)
    {
        xapi_parseOptionalDouble(svgNode, "width", &viewWidth);
        xapi_parseOptionalDouble(svgNode, "height", &viewHeight);

        if (viewWidth <= 0.0) { viewWidth = (double)targetWidth; }
        if (viewHeight <= 0.0) { viewHeight = viewWidth; }
    }

    if (viewWidth <= 0.0 || viewHeight <= 0.0) { return false; }

    int targetHeight = xapi_roundToInt((double)targetWidth * viewHeight / viewWidth);
    if (targetHeight < 1) { targetHeight = 1; }

    *outMinX         = minX;
    *outMinY         = minY;
    *outViewWidth    = viewWidth;
    *outViewHeight   = viewHeight;
    *outTargetHeight = targetHeight;
    return true;
}

static void xapi_initBaseStyle(xapi_style *baseStyle)
{
    if (!baseStyle) { return; }
    baseStyle->fillEnabled   = true;
    baseStyle->fillColor     = xapi_packColor(0, 0, 0, 0xff);
    baseStyle->fillRuleEvenOdd = false;
    baseStyle->strokeEnabled = false;
    baseStyle->strokeColor   = xapi_packColor(0, 0, 0, 0xff);
    baseStyle->strokeWidth   = 1;
}

static void xapi_renderSvgRoot(xapi_renderContext *ctx, const xapi_XmlNode *svgNode)
{
    xapi_style baseStyle;
    xapi_initBaseStyle(&baseStyle);
    xapi_renderNode(ctx, svgNode, &baseStyle);
}

static int xapi_chooseSuperSampleFactor(int width)
{
    if (width <= 24) { return 4; }
    if (width <= 40) { return 3; }
    if (width <= 48) { return 2; }
    return 1;
}

static void xapi_downsampleAndBlit(
    SHEET_INFO          *sheetInfo,
    SHEET               *sheet,
    int                  startX,
    int                  startY,
    const SHEET_BUFFER *source,
    int                  sourceWidth,
    int                  sourceHeight,
    int                  targetWidth,
    int                  targetHeight)
{
    if (!sheetInfo || !sheet || !source || sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0)
    {
        return;
    }

    for (int y = 0; y < targetHeight; y++)
    {
        int sy0 = (y * sourceHeight) / targetHeight;
        int sy1 = ((y + 1) * sourceHeight) / targetHeight;
        if (sy1 <= sy0) { sy1 = sy0 + 1; }
        if (sy1 > sourceHeight) { sy1 = sourceHeight; }

        for (int x = 0; x < targetWidth; x++)
        {
            int sx0 = (x * sourceWidth) / targetWidth;
            int sx1 = ((x + 1) * sourceWidth) / targetWidth;
            if (sx1 <= sx0) { sx1 = sx0 + 1; }
            if (sx1 > sourceWidth) { sx1 = sourceWidth; }

            int sumR = 0;
            int sumG = 0;
            int sumB = 0;
            int sumA = 0;
            int count = 0;
            for (int sy = sy0; sy < sy1; sy++)
            {
                for (int sx = sx0; sx < sx1; sx++)
                {
                    const SHEET_BUFFER *pixel = &source[sy * sourceWidth + sx];
                    sumR += pixel->r;
                    sumG += pixel->g;
                    sumB += pixel->b;
                    sumA += pixel->a;
                    count++;
                }
            }
            if (count <= 0) { continue; }

            SHEET_BUFFER out;
            out.r = (uint8_t)((sumR + count / 2) / count);
            out.g = (uint8_t)((sumG + count / 2) / count);
            out.b = (uint8_t)((sumB + count / 2) / count);
            out.a = (uint8_t)((sumA + count / 2) / count);

            if (out.a == 0) { continue; }

            int dstX = startX + x;
            int dstY = startY + y;
            if (out.a < 255 && sheet->buffer && dstX >= 0 && dstY >= 0 && dstX < (int)sheet->width &&
                dstY < (int)sheet->height)
            {
                const SHEET_BUFFER *dstBuffer = (const SHEET_BUFFER *)sheet->buffer;
                const SHEET_BUFFER *dstPixel  = &dstBuffer[dstY * (int)sheet->width + dstX];
                int                 invA      = 255 - (int)out.a;

                out.r = (uint8_t)xapi_clampInt((int)out.r + ((int)dstPixel->r * invA + 127) / 255, 0, 255);
                out.g = (uint8_t)xapi_clampInt((int)out.g + ((int)dstPixel->g * invA + 127) / 255, 0, 255);
                out.b = (uint8_t)xapi_clampInt((int)out.b + ((int)dstPixel->b * invA + 127) / 255, 0, 255);
                out.a = dstPixel->a != 0 ? dstPixel->a : 0xff;
            }
            draw_point(sheetInfo, sheet, dstX, dstY, out);
        }
    }
}

int xapi_drawSvgBySheet(
    SHEET_INFO *sheetInfo,
    SHEET      *sheet,
    int         startX,
    int         startY,
    int         width,
    const char *svgText,
    bool        enableTrans)
{
    if (!sheetInfo || !sheet || !svgText || width <= 0) { return -1; }

    xapi_XmlTree tree;
    memset(&tree, 0, sizeof(tree));

    int parseRet = xapi_parseXml(svgText, &tree);
    if (parseRet != XAPI_XML_PARSE_OK || !tree.root)
    {
        xapi_freeXmlTree(&tree);
        return -1;
    }

    const xapi_XmlNode *svgNode = tree.root;
    if (!svgNode->name || strcmp(svgNode->name, "svg") != 0)
    {
        xapi_freeXmlTree(&tree);
        return -1;
    }

    double minX = 0.0, minY = 0.0, viewWidth = 0.0, viewHeight = 0.0;
    int    targetHeight = 0;
    if (!xapi_resolveViewMetrics(
            svgNode, width, &minX, &minY, &viewWidth, &viewHeight, &targetHeight))
    {
        xapi_freeXmlTree(&tree);
        return -1;
    }

    int superSampleFactor = xapi_chooseSuperSampleFactor(width);
    if (superSampleFactor > 1)
    {
        int superWidth = width * superSampleFactor;
        double superMinX = 0.0, superMinY = 0.0, superViewWidth = 0.0, superViewHeight = 0.0;
        int    superHeight = 0;

        if (xapi_resolveViewMetrics(
                svgNode, superWidth, &superMinX, &superMinY, &superViewWidth, &superViewHeight, &superHeight))
        {
            size_t pixelCount = (size_t)superWidth * (size_t)superHeight;
            if (pixelCount > 0 && pixelCount <= ((size_t)8 * 1024 * 1024))
            {
                SHEET_BUFFER *superBuffer = (SHEET_BUFFER *)malloc(pixelCount * sizeof(SHEET_BUFFER));
                if (superBuffer)
                {
                    memset(superBuffer, 0, pixelCount * sizeof(SHEET_BUFFER));

                    SHEET superSheet;
                    memset(&superSheet, 0, sizeof(superSheet));
                    superSheet.buffer = (void *)superBuffer;
                    superSheet.width  = (uint32_t)superWidth;
                    superSheet.height = (uint32_t)superHeight;

                    xapi_renderContext superCtx;
                    superCtx.sheetInfo    = sheetInfo;
                    superCtx.sheet        = &superSheet;
                    superCtx.startX       = 0;
                    superCtx.startY       = 0;
                    superCtx.viewBoxMinX  = superMinX;
                    superCtx.viewBoxMinY  = superMinY;
                    superCtx.scale        = (double)superWidth / superViewWidth;
                    superCtx.outputWidth  = superWidth;
                    superCtx.outputHeight = superHeight;
                    superCtx.enableTrans  = enableTrans;

                    xapi_renderSvgRoot(&superCtx, svgNode);
                    xapi_downsampleAndBlit(
                        sheetInfo, sheet, startX, startY, superBuffer, superWidth, superHeight, width, targetHeight);
                    free(superBuffer);
                    xapi_freeXmlTree(&tree);
                    return targetHeight;
                }
            }
        }
    }

    xapi_renderContext ctx;
    ctx.sheetInfo    = sheetInfo;
    ctx.sheet        = sheet;
    ctx.startX       = startX;
    ctx.startY       = startY;
    ctx.viewBoxMinX  = minX;
    ctx.viewBoxMinY  = minY;
    ctx.scale        = (double)width / viewWidth;
    ctx.outputWidth  = width;
    ctx.outputHeight = targetHeight;
    ctx.enableTrans  = enableTrans;

    xapi_renderSvgRoot(&ctx, svgNode);

    xapi_freeXmlTree(&tree);
    return targetHeight;
}

static bool xapi_isValidFaName(const char *name)
{
    if (!name || *name == '\0') { return false; }
    if (strstr(name, "..")) { return false; }

    for (const char *p = name; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\' || *p == ':') { return false; }
    }
    return true;
}

int xapi_drawFABySheet(
    SHEET_INFO *sheetInfo,
    SHEET      *sheet,
    int         startX,
    int         startY,
    int         width,
    const char *name,
    bool        enableTrans)
{
    if (!sheetInfo || !sheet || width <= 0 || !xapi_isValidFaName(name)) { return -1; }

    static const char *kSvgDir = "/system/resources/svg/";
    static const char *kSvgExt = ".svg";

    size_t nameLen = strlen(name);
    size_t pathLen = strlen(kSvgDir) + nameLen + strlen(kSvgExt);
    char  *path    = (char *)malloc(pathLen + 1);
    if (!path) { return -1; }

    memcpy(path, kSvgDir, strlen(kSvgDir));
    memcpy(path + strlen(kSvgDir), name, nameLen);
    memcpy(path + strlen(kSvgDir) + nameLen, kSvgExt, strlen(kSvgExt));
    path[pathLen] = '\0';

    vfs_node_t file = vfs_open(path);
    free(path);
    if (!file || file->size == 0 || file->size > (size_t)(2 * 1024 * 1024))
    {
        if (file) { vfs_close(file); }
        return -1;
    }

    size_t fileSize = file->size;
    char  *svgText  = (char *)malloc(fileSize + 1);
    if (!svgText)
    {
        vfs_close(file);
        return -1;
    }

    size_t readSize = vfs_read(file, (uint8_t *)svgText, 0, fileSize);
    vfs_close(file);
    if (readSize != fileSize)
    {
        free(svgText);
        return -1;
    }
    svgText[fileSize] = '\0';

    int ret = xapi_drawSvgBySheet(sheetInfo, sheet, startX, startY, width, svgText, enableTrans);
    free(svgText);
    return ret;
}
