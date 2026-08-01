#pragma once

#include "./liballoc/alloc.h"

// 一个用户态头文件竟然依赖系统源代码，你们怎么想的？？？？

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xapi_XmlNodeType
{
    XAPI_XML_NODE_ELEMENT = 1,
    XAPI_XML_NODE_TEXT    = 2
} xapi_XmlNodeType;

typedef struct xapi_XmlAttribute
{
    char                     *name;
    char                     *value;
    struct xapi_XmlAttribute *next;
} xapi_XmlAttribute;

typedef struct xapi_XmlNode
{
    xapi_XmlNodeType   type;
    char              *name;
    char              *text;
    xapi_XmlAttribute *attributes;

    struct xapi_XmlNode *parent;
    struct xapi_XmlNode *firstChild;
    struct xapi_XmlNode *lastChild;
    struct xapi_XmlNode *nextSibling;
} xapi_XmlNode;

typedef struct xapi_XmlTree
{
    xapi_XmlNode *root;
} xapi_XmlTree;

typedef enum xapi_XmlParseResult
{
    XAPI_XML_PARSE_OK               = 0,
    XAPI_XML_PARSE_INVALID_PARAM    = 1,
    XAPI_XML_PARSE_NO_MEMORY        = 2,
    XAPI_XML_PARSE_SYNTAX_ERROR     = 3,
    XAPI_XML_PARSE_MISMATCH_TAG     = 4,
    XAPI_XML_PARSE_TRAILING_CONTENT = 5
} xapi_XmlParseResult;

int xapi_parseXml(const char *xmlText, xapi_XmlTree *outTree);
void xapi_freeXmlNode(xapi_XmlNode *node);
void xapi_freeXmlTree(xapi_XmlTree *tree);

#ifdef __cplusplus
}
#endif

