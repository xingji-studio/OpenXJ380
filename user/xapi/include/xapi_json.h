#pragma once

#include "./liballoc/alloc.h"

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xapi_JsonValueType
{
    XAPI_JSON_VALUE_NULL   = 0,
    XAPI_JSON_VALUE_BOOL   = 1,
    XAPI_JSON_VALUE_NUMBER = 2,
    XAPI_JSON_VALUE_STRING = 3,
    XAPI_JSON_VALUE_ARRAY  = 4,
    XAPI_JSON_VALUE_OBJECT = 5
} xapi_JsonValueType;

typedef struct xapi_JsonValue xapi_JsonValue;

typedef struct xapi_JsonObjectEntry
{
    char                        *key;
    xapi_JsonValue              *value;
    struct xapi_JsonObjectEntry *next;
} xapi_JsonObjectEntry;

struct xapi_JsonValue
{
    xapi_JsonValueType type;

    union
    {
        int                  boolValue;
        double               numberValue;
        char                *stringValue;
        xapi_JsonValue      *arrayValue;
        xapi_JsonObjectEntry *objectValue;
    } as;

    struct xapi_JsonValue *next;
};

typedef struct xapi_JsonDocument
{
    xapi_JsonValue *root;
} xapi_JsonDocument;

typedef enum xapi_JsonParseResult
{
    XAPI_JSON_PARSE_OK               = 0,
    XAPI_JSON_PARSE_INVALID_PARAM    = 1,
    XAPI_JSON_PARSE_NO_MEMORY        = 2,
    XAPI_JSON_PARSE_SYNTAX_ERROR     = 3,
    XAPI_JSON_PARSE_INVALID_NUMBER   = 4,
    XAPI_JSON_PARSE_INVALID_STRING   = 5,
    XAPI_JSON_PARSE_TRAILING_CONTENT = 6
} xapi_JsonParseResult;

int             xapi_parseJson(const char *jsonText, xapi_JsonDocument *outDocument);
void            xapi_freeJsonValue(xapi_JsonValue *value);
void            xapi_freeJsonDocument(xapi_JsonDocument *document);
xapi_JsonValue *xapi_jsonObjectGet(const xapi_JsonValue *objectValue, const char *key);
int             xapi_jsonArraySize(const xapi_JsonValue *arrayValue);
xapi_JsonValue *xapi_jsonArrayAt(const xapi_JsonValue *arrayValue, int index);

#ifdef __cplusplus
}
#endif

