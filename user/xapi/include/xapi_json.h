#pragma once

#include "./liballoc/alloc.h"

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XJsonValueType
{
    XAPI_JSON_VALUE_NULL   = 0,
    XAPI_JSON_VALUE_BOOL   = 1,
    XAPI_JSON_VALUE_NUMBER = 2,
    XAPI_JSON_VALUE_STRING = 3,
    XAPI_JSON_VALUE_ARRAY  = 4,
    XAPI_JSON_VALUE_OBJECT = 5
} XJsonValueType;

typedef struct XJsonValue XJsonValue;

typedef struct XJsonObjectEntry
{
    char                    *key;
    XJsonValue              *value;
    struct XJsonObjectEntry *next;
} XJsonObjectEntry;

struct XJsonValue
{
    XJsonValueType type;

    union
    {
        int               boolValue;
        double            numberValue;
        char             *stringValue;
        XJsonValue       *arrayValue;
        XJsonObjectEntry *objectValue;
    } as;

    struct XJsonValue *next;
};

typedef struct XJsonDocument
{
    XJsonValue *root;
} XJsonDocument;

typedef enum XJsonParseResult
{
    XAPI_JSON_PARSE_OK               = 0,
    XAPI_JSON_PARSE_INVALID_PARAM    = 1,
    XAPI_JSON_PARSE_NO_MEMORY        = 2,
    XAPI_JSON_PARSE_SYNTAX_ERROR     = 3,
    XAPI_JSON_PARSE_INVALID_NUMBER   = 4,
    XAPI_JSON_PARSE_INVALID_STRING   = 5,
    XAPI_JSON_PARSE_TRAILING_CONTENT = 6
} XJsonParseResult;

int         xapi_parseJson(const char *jsonText, XJsonDocument *outDocument);
void        xapi_freeJsonValue(XJsonValue *value);
void        xapi_freeJsonDocument(XJsonDocument *document);
XJsonValue *xapi_jsonObjectGet(const XJsonValue *objectValue, const char *key);
int         xapi_jsonArraySize(const XJsonValue *arrayValue);
XJsonValue *xapi_jsonArrayAt(const XJsonValue *arrayValue, int index);

#ifdef __cplusplus
}
#endif

