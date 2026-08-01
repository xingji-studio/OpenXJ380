typedef struct xapi_XmlParser
{
    const char *cursor;
} xapi_XmlParser;

static int xapi_xmlIsNameStart(char ch)
{
    return ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == ':');
}

static int xapi_xmlIsNameChar(char ch)
{
    return xapi_xmlIsNameStart(ch) || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
}

static int xapi_xmlStartsWith(const char *text, const char *pattern)
{
    while (*pattern)
    {
        if (*text != *pattern) { return 0; }
        text++;
        pattern++;
    }
    return 1;
}

static void xapi_xmlSkipSpaces(xapi_XmlParser *parser)
{
    while (*parser->cursor && isspace((unsigned char)*parser->cursor))
    {
        parser->cursor++;
    }
}

static char *xapi_xmlDupRange(const char *begin, const char *end)
{
    if (end < begin) { return NULL; }

    size_t len = (size_t)(end - begin);
    char  *out = (char *)malloc(len + 1);
    if (!out) { return NULL; }

    if (len != 0) { memcpy(out, begin, len); }
    out[len] = '\0';
    return out;
}

static xapi_XmlNode *xapi_xmlCreateNode(xapi_XmlNodeType type)
{
    xapi_XmlNode *node = (xapi_XmlNode *)calloc(1, sizeof(xapi_XmlNode));
    if (!node) { return NULL; }
    node->type = type;
    return node;
}

static int xapi_xmlAppendChild(xapi_XmlNode *parent, xapi_XmlNode *child)
{
    if (!parent || !child) { return XAPI_XML_PARSE_INVALID_PARAM; }

    child->parent = parent;
    if (!parent->firstChild)
    {
        parent->firstChild = child;
        parent->lastChild  = child;
    }
    else
    {
        parent->lastChild->nextSibling = child;
        parent->lastChild              = child;
    }
    return XAPI_XML_PARSE_OK;
}

static int xapi_xmlIsAllSpaces(const char *begin, const char *end)
{
    const char *it = begin;
    while (it < end)
    {
        if (!isspace((unsigned char)*it)) { return 0; }
        it++;
    }
    return 1;
}

static void xapi_xmlFreeAttributes(xapi_XmlAttribute *attr)
{
    while (attr)
    {
        xapi_XmlAttribute *next = attr->next;
        free(attr->name);
        free(attr->value);
        free(attr);
        attr = next;
    }
}

void xapi_freeXmlNode(xapi_XmlNode *node)
{
    if (!node) { return; }

    xapi_XmlNode *child = node->firstChild;
    while (child)
    {
        xapi_XmlNode *next = child->nextSibling;
        xapi_freeXmlNode(child);
        child = next;
    }

    xapi_xmlFreeAttributes(node->attributes);
    free(node->name);
    free(node->text);
    free(node);
}

void xapi_freeXmlTree(xapi_XmlTree *tree)
{
    if (!tree) { return; }
    xapi_freeXmlNode(tree->root);
    tree->root = NULL;
}

static int xapi_xmlSkipComment(xapi_XmlParser *parser)
{
    const char *p = parser->cursor + 4;
    while (*p)
    {
        if (p[0] == '-' && p[1] == '-' && p[2] == '>')
        {
            parser->cursor = p + 3;
            return XAPI_XML_PARSE_OK;
        }
        p++;
    }
    return XAPI_XML_PARSE_SYNTAX_ERROR;
}

static int xapi_xmlSkipInstruction(xapi_XmlParser *parser)
{
    const char *p = parser->cursor + 2;
    while (*p)
    {
        if (p[0] == '?' && p[1] == '>')
        {
            parser->cursor = p + 2;
            return XAPI_XML_PARSE_OK;
        }
        p++;
    }
    return XAPI_XML_PARSE_SYNTAX_ERROR;
}

static int xapi_xmlSkipDeclaration(xapi_XmlParser *parser)
{
    const char *p           = parser->cursor + 2;
    int         squareDepth = 0;
    char        quote       = 0;

    while (*p)
    {
        char ch = *p;
        if (quote)
        {
            if (ch == quote) { quote = 0; }
            p++;
            continue;
        }

        if (ch == '"' || ch == '\'')
        {
            quote = ch;
            p++;
            continue;
        }

        if (ch == '[')
        {
            squareDepth++;
            p++;
            continue;
        }

        if (ch == ']' && squareDepth > 0)
        {
            squareDepth--;
            p++;
            continue;
        }

        if (ch == '>' && squareDepth == 0)
        {
            parser->cursor = p + 1;
            return XAPI_XML_PARSE_OK;
        }

        p++;
    }

    return XAPI_XML_PARSE_SYNTAX_ERROR;
}

static int xapi_xmlSkipMisc(xapi_XmlParser *parser)
{
    int ret = XAPI_XML_PARSE_OK;

    for (;;)
    {
        xapi_xmlSkipSpaces(parser);

        if (xapi_xmlStartsWith(parser->cursor, "<!--"))
        {
            ret = xapi_xmlSkipComment(parser);
            if (ret != XAPI_XML_PARSE_OK) { return ret; }
            continue;
        }

        if (xapi_xmlStartsWith(parser->cursor, "<?"))
        {
            ret = xapi_xmlSkipInstruction(parser);
            if (ret != XAPI_XML_PARSE_OK) { return ret; }
            continue;
        }

        if (xapi_xmlStartsWith(parser->cursor, "<!"))
        {
            ret = xapi_xmlSkipDeclaration(parser);
            if (ret != XAPI_XML_PARSE_OK) { return ret; }
            continue;
        }

        break;
    }

    return ret;
}

static int xapi_xmlParseTextNode(xapi_XmlParser *parser, xapi_XmlNode *parent)
{
    const char *begin = parser->cursor;
    const char *end   = begin;

    while (*end && *end != '<')
    {
        end++;
    }

    parser->cursor = end;
    if (begin == end || xapi_xmlIsAllSpaces(begin, end)) { return XAPI_XML_PARSE_OK; }

    xapi_XmlNode *textNode = xapi_xmlCreateNode(XAPI_XML_NODE_TEXT);
    if (!textNode) { return XAPI_XML_PARSE_NO_MEMORY; }

    textNode->text = xapi_xmlDupRange(begin, end);
    if (!textNode->text)
    {
        xapi_freeXmlNode(textNode);
        return XAPI_XML_PARSE_NO_MEMORY;
    }

    return xapi_xmlAppendChild(parent, textNode);
}

static int xapi_xmlParseCDataNode(xapi_XmlParser *parser, xapi_XmlNode *parent)
{
    const char *begin = parser->cursor + 9;
    const char *p     = begin;

    while (*p)
    {
        if (p[0] == ']' && p[1] == ']' && p[2] == '>')
        {
            xapi_XmlNode *textNode = xapi_xmlCreateNode(XAPI_XML_NODE_TEXT);
            if (!textNode) { return XAPI_XML_PARSE_NO_MEMORY; }

            textNode->text = xapi_xmlDupRange(begin, p);
            if (!textNode->text)
            {
                xapi_freeXmlNode(textNode);
                return XAPI_XML_PARSE_NO_MEMORY;
            }

            parser->cursor = p + 3;
            return xapi_xmlAppendChild(parent, textNode);
        }
        p++;
    }

    return XAPI_XML_PARSE_SYNTAX_ERROR;
}

static int xapi_xmlParseName(xapi_XmlParser *parser, char **outName)
{
    const char *begin = parser->cursor;
    const char *p     = begin;

    if (!xapi_xmlIsNameStart(*p)) { return XAPI_XML_PARSE_SYNTAX_ERROR; }

    while (*p && xapi_xmlIsNameChar(*p))
    {
        p++;
    }

    *outName = xapi_xmlDupRange(begin, p);
    if (!*outName) { return XAPI_XML_PARSE_NO_MEMORY; }

    parser->cursor = p;
    return XAPI_XML_PARSE_OK;
}

static int xapi_xmlParseAttribute(xapi_XmlParser *parser, xapi_XmlNode *node)
{
    xapi_XmlAttribute *attr = (xapi_XmlAttribute *)calloc(1, sizeof(xapi_XmlAttribute));
    if (!attr) { return XAPI_XML_PARSE_NO_MEMORY; }

    int ret = xapi_xmlParseName(parser, &attr->name);
    if (ret != XAPI_XML_PARSE_OK)
    {
        xapi_xmlFreeAttributes(attr);
        return ret;
    }

    xapi_xmlSkipSpaces(parser);
    if (*parser->cursor != '=')
    {
        xapi_xmlFreeAttributes(attr);
        return XAPI_XML_PARSE_SYNTAX_ERROR;
    }
    parser->cursor++;

    xapi_xmlSkipSpaces(parser);
    if (*parser->cursor != '"' && *parser->cursor != '\'')
    {
        xapi_xmlFreeAttributes(attr);
        return XAPI_XML_PARSE_SYNTAX_ERROR;
    }

    char        quote = *parser->cursor;
    const char *begin = ++parser->cursor;
    while (*parser->cursor && *parser->cursor != quote)
    {
        parser->cursor++;
    }

    if (*parser->cursor != quote)
    {
        xapi_xmlFreeAttributes(attr);
        return XAPI_XML_PARSE_SYNTAX_ERROR;
    }

    attr->value = xapi_xmlDupRange(begin, parser->cursor);
    if (!attr->value)
    {
        xapi_xmlFreeAttributes(attr);
        return XAPI_XML_PARSE_NO_MEMORY;
    }

    parser->cursor++;

    if (!node->attributes)
    {
        node->attributes = attr;
    }
    else
    {
        xapi_XmlAttribute *tail = node->attributes;
        while (tail->next)
        {
            tail = tail->next;
        }
        tail->next = attr;
    }

    return XAPI_XML_PARSE_OK;
}

static int xapi_xmlParseElement(xapi_XmlParser *parser, xapi_XmlNode **outNode)
{
    if (*parser->cursor != '<') { return XAPI_XML_PARSE_SYNTAX_ERROR; }
    parser->cursor++;

    xapi_XmlNode *node = xapi_xmlCreateNode(XAPI_XML_NODE_ELEMENT);
    if (!node) { return XAPI_XML_PARSE_NO_MEMORY; }

    int ret = xapi_xmlParseName(parser, &node->name);
    if (ret != XAPI_XML_PARSE_OK)
    {
        xapi_freeXmlNode(node);
        return ret;
    }

    for (;;)
    {
        xapi_xmlSkipSpaces(parser);

        if (*parser->cursor == '/' && parser->cursor[1] == '>')
        {
            parser->cursor += 2;
            *outNode = node;
            return XAPI_XML_PARSE_OK;
        }

        if (*parser->cursor == '>')
        {
            parser->cursor++;
            break;
        }

        if (*parser->cursor == '\0')
        {
            xapi_freeXmlNode(node);
            return XAPI_XML_PARSE_SYNTAX_ERROR;
        }

        ret = xapi_xmlParseAttribute(parser, node);
        if (ret != XAPI_XML_PARSE_OK)
        {
            xapi_freeXmlNode(node);
            return ret;
        }
    }

    for (;;)
    {
        if (xapi_xmlStartsWith(parser->cursor, "</"))
        {
            char *closingName = NULL;

            parser->cursor += 2;
            ret = xapi_xmlParseName(parser, &closingName);
            if (ret != XAPI_XML_PARSE_OK)
            {
                xapi_freeXmlNode(node);
                return ret;
            }

            if (strcmp(node->name, closingName) != 0)
            {
                free(closingName);
                xapi_freeXmlNode(node);
                return XAPI_XML_PARSE_MISMATCH_TAG;
            }

            free(closingName);
            xapi_xmlSkipSpaces(parser);
            if (*parser->cursor != '>')
            {
                xapi_freeXmlNode(node);
                return XAPI_XML_PARSE_SYNTAX_ERROR;
            }

            parser->cursor++;
            *outNode = node;
            return XAPI_XML_PARSE_OK;
        }

        if (xapi_xmlStartsWith(parser->cursor, "<!--"))
        {
            ret = xapi_xmlSkipComment(parser);
            if (ret != XAPI_XML_PARSE_OK)
            {
                xapi_freeXmlNode(node);
                return ret;
            }
            continue;
        }

        if (xapi_xmlStartsWith(parser->cursor, "<![CDATA["))
        {
            ret = xapi_xmlParseCDataNode(parser, node);
            if (ret != XAPI_XML_PARSE_OK)
            {
                xapi_freeXmlNode(node);
                return ret;
            }
            continue;
        }

        if (xapi_xmlStartsWith(parser->cursor, "<?"))
        {
            ret = xapi_xmlSkipInstruction(parser);
            if (ret != XAPI_XML_PARSE_OK)
            {
                xapi_freeXmlNode(node);
                return ret;
            }
            continue;
        }

        if (*parser->cursor == '<')
        {
            xapi_XmlNode *child = NULL;
            ret                 = xapi_xmlParseElement(parser, &child);
            if (ret != XAPI_XML_PARSE_OK)
            {
                xapi_freeXmlNode(node);
                return ret;
            }
            xapi_xmlAppendChild(node, child);
            continue;
        }

        ret = xapi_xmlParseTextNode(parser, node);
        if (ret != XAPI_XML_PARSE_OK)
        {
            xapi_freeXmlNode(node);
            return ret;
        }

        if (*parser->cursor == '\0')
        {
            xapi_freeXmlNode(node);
            return XAPI_XML_PARSE_SYNTAX_ERROR;
        }
    }
}

int xapi_parseXml(const char *xmlText, xapi_XmlTree *outTree)
{
    if (!xmlText || !outTree) { return XAPI_XML_PARSE_INVALID_PARAM; }

    outTree->root = NULL;

    xapi_XmlParser parser;
    parser.cursor = xmlText;

    int ret = xapi_xmlSkipMisc(&parser);
    if (ret != XAPI_XML_PARSE_OK) { return ret; }

    xapi_XmlNode *root = NULL;
    ret                = xapi_xmlParseElement(&parser, &root);
    if (ret != XAPI_XML_PARSE_OK) { return ret; }

    outTree->root = root;
    ret           = xapi_xmlSkipMisc(&parser);
    if (ret != XAPI_XML_PARSE_OK)
    {
        xapi_freeXmlTree(outTree);
        return ret;
    }

    if (*parser.cursor != '\0')
    {
        xapi_freeXmlTree(outTree);
        return XAPI_XML_PARSE_TRAILING_CONTENT;
    }

    return XAPI_XML_PARSE_OK;
}
