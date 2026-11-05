typedef struct XJsonParser
{
    const char *cursor;
} XJsonParser;

static void xapi_jsonSkipSpaces(XJsonParser *parser)
{
    while (*parser->cursor && isspace((unsigned char)*parser->cursor))
    {
        parser->cursor++;
    }
}

static int xapi_jsonStartsWith(const char *text, const char *pattern)
{
    while (*pattern)
    {
        if (*text != *pattern) { return 0; }
        text++;
        pattern++;
    }
    return 1;
}

static XJsonValue *xapi_jsonCreateValue(XJsonValueType type)
{
    XJsonValue *value = (XJsonValue *)calloc(1, sizeof(XJsonValue));
    if (!value) { return NULL; }
    value->type = type;
    return value;
}

static void xapi_jsonFreeObjectEntries(XJsonObjectEntry *entry)
{
    while (entry)
    {
        XJsonObjectEntry *next = entry->next;
        free(entry->key);
        xapi_freeJsonValue(entry->value);
        free(entry);
        entry = next;
    }
}

void xapi_freeJsonValue(XJsonValue *value)
{
    while (value)
    {
        XJsonValue *next = value->next;

        if (value->type == XAPI_JSON_VALUE_STRING)
        {
            free(value->as.stringValue);
        }
        else if (value->type == XAPI_JSON_VALUE_ARRAY)
        {
            xapi_freeJsonValue(value->as.arrayValue);
        }
        else if (value->type == XAPI_JSON_VALUE_OBJECT)
        {
            xapi_jsonFreeObjectEntries(value->as.objectValue);
        }

        free(value);
        value = next;
    }
}

void xapi_freeJsonDocument(XJsonDocument *document)
{
    if (!document) { return; }

    xapi_freeJsonValue(document->root);
    document->root = NULL;
}

static int xapi_jsonAppendArrayItem(XJsonValue *arrayValue, XJsonValue *item)
{
    if (!arrayValue || !item || arrayValue->type != XAPI_JSON_VALUE_ARRAY) { return XAPI_JSON_PARSE_INVALID_PARAM; }

    if (!arrayValue->as.arrayValue)
    {
        arrayValue->as.arrayValue = item;
        return XAPI_JSON_PARSE_OK;
    }

    XJsonValue *tail = arrayValue->as.arrayValue;
    while (tail->next)
    {
        tail = tail->next;
    }
    tail->next = item;
    return XAPI_JSON_PARSE_OK;
}

static int xapi_jsonAppendObjectEntry(XJsonValue *objectValue, XJsonObjectEntry *entry)
{
    if (!objectValue || !entry || objectValue->type != XAPI_JSON_VALUE_OBJECT) { return XAPI_JSON_PARSE_INVALID_PARAM; }

    if (!objectValue->as.objectValue)
    {
        objectValue->as.objectValue = entry;
        return XAPI_JSON_PARSE_OK;
    }

    XJsonObjectEntry *tail = objectValue->as.objectValue;
    while (tail->next)
    {
        tail = tail->next;
    }
    tail->next = entry;
    return XAPI_JSON_PARSE_OK;
}

static int xapi_jsonHexValue(char ch)
{
    if (ch >= '0' && ch <= '9') { return ch - '0'; }
    if (ch >= 'a' && ch <= 'f') { return ch - 'a' + 10; }
    if (ch >= 'A' && ch <= 'F') { return ch - 'A' + 10; }
    return -1;
}

static int xapi_jsonAppendChar(char **buffer, size_t *length, size_t *capacity, char ch)
{
    if (*length + 1 >= *capacity)
    {
        size_t newCapacity = (*capacity == 0) ? 16 : (*capacity * 2);
        char  *newBuffer   = (char *)malloc(newCapacity);
        if (!newBuffer) { return XAPI_JSON_PARSE_NO_MEMORY; }

        if (*buffer && *length != 0) { memcpy(newBuffer, *buffer, *length); }
        free(*buffer);
        *buffer   = newBuffer;
        *capacity = newCapacity;
    }

    (*buffer)[*length] = ch;
    (*length)++;
    return XAPI_JSON_PARSE_OK;
}

static int xapi_jsonParseStringRaw(XJsonParser *parser, char **outString)
{
    char  *buffer   = NULL;
    size_t length   = 0;
    size_t capacity = 0;

    if (*parser->cursor != '"') { return XAPI_JSON_PARSE_INVALID_STRING; }
    parser->cursor++;

    while (*parser->cursor)
    {
        char ch = *parser->cursor++;
        if (ch == '"')
        {
            int ret = xapi_jsonAppendChar(&buffer, &length, &capacity, '\0');
            if (ret != XAPI_JSON_PARSE_OK)
            {
                free(buffer);
                return ret;
            }

            *outString = buffer;
            return XAPI_JSON_PARSE_OK;
        }

        if ((unsigned char)ch < 0x20)
        {
            free(buffer);
            return XAPI_JSON_PARSE_INVALID_STRING;
        }

        if (ch != '\\')
        {
            int ret = xapi_jsonAppendChar(&buffer, &length, &capacity, ch);
            if (ret != XAPI_JSON_PARSE_OK)
            {
                free(buffer);
                return ret;
            }
            continue;
        }

        char escaped = *parser->cursor++;
        char decoded = 0;
        switch (escaped)
        {
        case '"': decoded = '"'; break;
        case '\\': decoded = '\\'; break;
        case '/': decoded = '/'; break;
        case 'b': decoded = '\b'; break;
        case 'f': decoded = '\f'; break;
        case 'n': decoded = '\n'; break;
        case 'r': decoded = '\r'; break;
        case 't': decoded = '\t'; break;
        case 'u':
        {
            int codepoint = 0;
            int i         = 0;
            for (i = 0; i < 4; i++)
            {
                int value = xapi_jsonHexValue(parser->cursor[i]);
                if (value < 0)
                {
                    free(buffer);
                    return XAPI_JSON_PARSE_INVALID_STRING;
                }
                codepoint = (codepoint << 4) | value;
            }
            parser->cursor += 4;
            decoded = (codepoint >= 0 && codepoint <= 0x7f) ? (char)codepoint : '?';
            break;
        }
        default:
            free(buffer);
            return XAPI_JSON_PARSE_INVALID_STRING;
        }

        int ret = xapi_jsonAppendChar(&buffer, &length, &capacity, decoded);
        if (ret != XAPI_JSON_PARSE_OK)
        {
            free(buffer);
            return ret;
        }
    }

    free(buffer);
    return XAPI_JSON_PARSE_INVALID_STRING;
}

static int xapi_jsonParseNumberValue(const char *begin, const char *end, double *outNumber)
{
    const char *p         = begin;
    int         negative  = 0;
    double      integer   = 0.0;
    double      fraction  = 0.0;
    double      divisor   = 1.0;
    int         exponent  = 0;
    int         expSign   = 1;

    if (p >= end) { return XAPI_JSON_PARSE_INVALID_NUMBER; }

    if (*p == '-')
    {
        negative = 1;
        p++;
    }

    if (p >= end) { return XAPI_JSON_PARSE_INVALID_NUMBER; }

    if (*p == '0')
    {
        p++;
        if (p < end && *p >= '0' && *p <= '9') { return XAPI_JSON_PARSE_INVALID_NUMBER; }
    }
    else if (*p >= '1' && *p <= '9')
    {
        while (p < end && *p >= '0' && *p <= '9')
        {
            integer = integer * 10.0 + (double)(*p - '0');
            p++;
        }
    }
    else
    {
        return XAPI_JSON_PARSE_INVALID_NUMBER;
    }

    if (begin[negative] == '0') { integer = 0.0; }

    if (p < end && *p == '.')
    {
        p++;
        if (p >= end || *p < '0' || *p > '9') { return XAPI_JSON_PARSE_INVALID_NUMBER; }

        while (p < end && *p >= '0' && *p <= '9')
        {
            fraction = fraction * 10.0 + (double)(*p - '0');
            divisor *= 10.0;
            p++;
        }
    }

    if (p < end && (*p == 'e' || *p == 'E'))
    {
        p++;
        if (p < end && (*p == '+' || *p == '-'))
        {
            if (*p == '-') { expSign = -1; }
            p++;
        }

        if (p >= end || *p < '0' || *p > '9') { return XAPI_JSON_PARSE_INVALID_NUMBER; }

        while (p < end && *p >= '0' && *p <= '9')
        {
            exponent = exponent * 10 + (*p - '0');
            p++;
        }
    }

    if (p != end) { return XAPI_JSON_PARSE_INVALID_NUMBER; }

    double value = integer + (fraction / divisor);
    if (negative) { value = -value; }

    while (exponent > 0)
    {
        value = (expSign > 0) ? (value * 10.0) : (value / 10.0);
        exponent--;
    }

    *outNumber = value;
    return XAPI_JSON_PARSE_OK;
}

static int xapi_jsonParseValue(XJsonParser *parser, XJsonValue **outValue);

static int xapi_jsonParseObject(XJsonParser *parser, XJsonValue **outValue)
{
    XJsonValue *objectValue = NULL;

    if (*parser->cursor != '{') { return XAPI_JSON_PARSE_SYNTAX_ERROR; }
    parser->cursor++;

    objectValue = xapi_jsonCreateValue(XAPI_JSON_VALUE_OBJECT);
    if (!objectValue) { return XAPI_JSON_PARSE_NO_MEMORY; }

    xapi_jsonSkipSpaces(parser);
    if (*parser->cursor == '}')
    {
        parser->cursor++;
        *outValue = objectValue;
        return XAPI_JSON_PARSE_OK;
    }

    for (;;)
    {
        XJsonObjectEntry *entry = NULL;
        int                   ret   = XAPI_JSON_PARSE_OK;

        xapi_jsonSkipSpaces(parser);
        if (*parser->cursor != '"')
        {
            xapi_freeJsonValue(objectValue);
            return XAPI_JSON_PARSE_SYNTAX_ERROR;
        }

        entry = (XJsonObjectEntry *)calloc(1, sizeof(XJsonObjectEntry));
        if (!entry)
        {
            xapi_freeJsonValue(objectValue);
            return XAPI_JSON_PARSE_NO_MEMORY;
        }

        ret = xapi_jsonParseStringRaw(parser, &entry->key);
        if (ret != XAPI_JSON_PARSE_OK)
        {
            free(entry);
            xapi_freeJsonValue(objectValue);
            return ret;
        }

        xapi_jsonSkipSpaces(parser);
        if (*parser->cursor != ':')
        {
            free(entry->key);
            free(entry);
            xapi_freeJsonValue(objectValue);
            return XAPI_JSON_PARSE_SYNTAX_ERROR;
        }
        parser->cursor++;

        xapi_jsonSkipSpaces(parser);
        ret = xapi_jsonParseValue(parser, &entry->value);
        if (ret != XAPI_JSON_PARSE_OK)
        {
            free(entry->key);
            free(entry);
            xapi_freeJsonValue(objectValue);
            return ret;
        }

        xapi_jsonAppendObjectEntry(objectValue, entry);

        xapi_jsonSkipSpaces(parser);
        if (*parser->cursor == '}')
        {
            parser->cursor++;
            *outValue = objectValue;
            return XAPI_JSON_PARSE_OK;
        }

        if (*parser->cursor != ',')
        {
            xapi_freeJsonValue(objectValue);
            return XAPI_JSON_PARSE_SYNTAX_ERROR;
        }
        parser->cursor++;
    }
}

static int xapi_jsonParseArray(XJsonParser *parser, XJsonValue **outValue)
{
    XJsonValue *arrayValue = NULL;

    if (*parser->cursor != '[') { return XAPI_JSON_PARSE_SYNTAX_ERROR; }
    parser->cursor++;

    arrayValue = xapi_jsonCreateValue(XAPI_JSON_VALUE_ARRAY);
    if (!arrayValue) { return XAPI_JSON_PARSE_NO_MEMORY; }

    xapi_jsonSkipSpaces(parser);
    if (*parser->cursor == ']')
    {
        parser->cursor++;
        *outValue = arrayValue;
        return XAPI_JSON_PARSE_OK;
    }

    for (;;)
    {
        XJsonValue *item = NULL;
        int             ret  = XAPI_JSON_PARSE_OK;

        xapi_jsonSkipSpaces(parser);
        ret = xapi_jsonParseValue(parser, &item);
        if (ret != XAPI_JSON_PARSE_OK)
        {
            xapi_freeJsonValue(arrayValue);
            return ret;
        }

        xapi_jsonAppendArrayItem(arrayValue, item);

        xapi_jsonSkipSpaces(parser);
        if (*parser->cursor == ']')
        {
            parser->cursor++;
            *outValue = arrayValue;
            return XAPI_JSON_PARSE_OK;
        }

        if (*parser->cursor != ',')
        {
            xapi_freeJsonValue(arrayValue);
            return XAPI_JSON_PARSE_SYNTAX_ERROR;
        }
        parser->cursor++;
    }
}

static int xapi_jsonParseNumber(XJsonParser *parser, XJsonValue **outValue)
{
    const char     *begin = parser->cursor;
    XJsonValue *value = NULL;
    double          numberValue;

    if (*parser->cursor == '-') { parser->cursor++; }

    if (*parser->cursor == '0')
    {
        parser->cursor++;
    }
    else if (*parser->cursor >= '1' && *parser->cursor <= '9')
    {
        while (*parser->cursor >= '0' && *parser->cursor <= '9')
        {
            parser->cursor++;
        }
    }
    else
    {
        return XAPI_JSON_PARSE_INVALID_NUMBER;
    }

    if (*parser->cursor == '.')
    {
        parser->cursor++;
        if (*parser->cursor < '0' || *parser->cursor > '9') { return XAPI_JSON_PARSE_INVALID_NUMBER; }
        while (*parser->cursor >= '0' && *parser->cursor <= '9')
        {
            parser->cursor++;
        }
    }

    if (*parser->cursor == 'e' || *parser->cursor == 'E')
    {
        parser->cursor++;
        if (*parser->cursor == '+' || *parser->cursor == '-') { parser->cursor++; }
        if (*parser->cursor < '0' || *parser->cursor > '9') { return XAPI_JSON_PARSE_INVALID_NUMBER; }
        while (*parser->cursor >= '0' && *parser->cursor <= '9')
        {
            parser->cursor++;
        }
    }

    int ret = xapi_jsonParseNumberValue(begin, parser->cursor, &numberValue);
    if (ret != XAPI_JSON_PARSE_OK) { return ret; }

    value = xapi_jsonCreateValue(XAPI_JSON_VALUE_NUMBER);
    if (!value) { return XAPI_JSON_PARSE_NO_MEMORY; }

    value->as.numberValue = numberValue;
    *outValue             = value;
    return XAPI_JSON_PARSE_OK;
}

static int xapi_jsonParseLiteral(XJsonParser *parser, XJsonValue **outValue)
{
    XJsonValue *value = NULL;

    if (xapi_jsonStartsWith(parser->cursor, "null"))
    {
        parser->cursor += 4;
        value = xapi_jsonCreateValue(XAPI_JSON_VALUE_NULL);
    }
    else if (xapi_jsonStartsWith(parser->cursor, "true"))
    {
        parser->cursor += 4;
        value = xapi_jsonCreateValue(XAPI_JSON_VALUE_BOOL);
        if (value) { value->as.boolValue = 1; }
    }
    else if (xapi_jsonStartsWith(parser->cursor, "false"))
    {
        parser->cursor += 5;
        value = xapi_jsonCreateValue(XAPI_JSON_VALUE_BOOL);
        if (value) { value->as.boolValue = 0; }
    }
    else
    {
        return XAPI_JSON_PARSE_SYNTAX_ERROR;
    }

    if (!value) { return XAPI_JSON_PARSE_NO_MEMORY; }

    *outValue = value;
    return XAPI_JSON_PARSE_OK;
}

static int xapi_jsonParseStringValue(XJsonParser *parser, XJsonValue **outValue)
{
    XJsonValue *value = xapi_jsonCreateValue(XAPI_JSON_VALUE_STRING);
    if (!value) { return XAPI_JSON_PARSE_NO_MEMORY; }

    int ret = xapi_jsonParseStringRaw(parser, &value->as.stringValue);
    if (ret != XAPI_JSON_PARSE_OK)
    {
        xapi_freeJsonValue(value);
        return ret;
    }

    *outValue = value;
    return XAPI_JSON_PARSE_OK;
}

static int xapi_jsonParseValue(XJsonParser *parser, XJsonValue **outValue)
{
    xapi_jsonSkipSpaces(parser);

    if (*parser->cursor == '{') { return xapi_jsonParseObject(parser, outValue); }
    if (*parser->cursor == '[') { return xapi_jsonParseArray(parser, outValue); }
    if (*parser->cursor == '"') { return xapi_jsonParseStringValue(parser, outValue); }
    if (*parser->cursor == '-' || (*parser->cursor >= '0' && *parser->cursor <= '9'))
    {
        return xapi_jsonParseNumber(parser, outValue);
    }
    if (*parser->cursor == 't' || *parser->cursor == 'f' || *parser->cursor == 'n')
    {
        return xapi_jsonParseLiteral(parser, outValue);
    }

    return XAPI_JSON_PARSE_SYNTAX_ERROR;
}

int xapi_parseJson(const char *jsonText, XJsonDocument *outDocument)
{
    XJsonParser parser;
    int             ret = XAPI_JSON_PARSE_OK;

    if (!jsonText || !outDocument) { return XAPI_JSON_PARSE_INVALID_PARAM; }

    outDocument->root = NULL;
    parser.cursor     = jsonText;

    xapi_jsonSkipSpaces(&parser);
    ret = xapi_jsonParseValue(&parser, &outDocument->root);
    if (ret != XAPI_JSON_PARSE_OK) { return ret; }

    xapi_jsonSkipSpaces(&parser);
    if (*parser.cursor != '\0')
    {
        xapi_freeJsonDocument(outDocument);
        return XAPI_JSON_PARSE_TRAILING_CONTENT;
    }

    return XAPI_JSON_PARSE_OK;
}

XJsonValue *xapi_jsonObjectGet(const XJsonValue *objectValue, const char *key)
{
    XJsonObjectEntry *entry;

    if (!objectValue || objectValue->type != XAPI_JSON_VALUE_OBJECT || !key) { return NULL; }

    entry = objectValue->as.objectValue;
    while (entry)
    {
        if (strcmp(entry->key, key) == 0) { return entry->value; }
        entry = entry->next;
    }
    return NULL;
}

int xapi_jsonArraySize(const XJsonValue *arrayValue)
{
    int             count = 0;
    XJsonValue *item;

    if (!arrayValue || arrayValue->type != XAPI_JSON_VALUE_ARRAY) { return 0; }

    item = arrayValue->as.arrayValue;
    while (item)
    {
        count++;
        item = item->next;
    }
    return count;
}

XJsonValue *xapi_jsonArrayAt(const XJsonValue *arrayValue, int index)
{
    int             current = 0;
    XJsonValue *item;

    if (!arrayValue || arrayValue->type != XAPI_JSON_VALUE_ARRAY || index < 0) { return NULL; }

    item = arrayValue->as.arrayValue;
    while (item)
    {
        if (current == index) { return item; }
        current++;
        item = item->next;
    }
    return NULL;
}
