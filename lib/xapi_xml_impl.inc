typedef struct XXmlParser
{
    const char *cursor;
} XXmlParser;

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

static void xapi_xmlSkipSpaces(XXmlParser *parser)
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

static XXmlNode *xapi_xmlCreateNode(XXmlNodeType type)
{
    XXmlNode *node = (XXmlNode *)calloc(1, sizeof(XXmlNode));
    if (!node) { return NULL; }
    node->type = type;
    return node;
}

static int xapi_xmlAppendChild(XXmlNode *parent, XXmlNode *child)
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

static void xapi_xmlFreeAttributes(XXmlAttribute *attr)
{
    while (attr)
    {
        XXmlAttribute *next = attr->next;
        free(attr->name);
        free(attr->value);
        free(attr);
        attr = next;
    }
}

void xapi_freeXmlNode(XXmlNode *node)
{
    if (!node) { return; }

    XXmlNode *child = node->firstChild;
    while (child)
    {
        XXmlNode *next = child->nextSibling;
        xapi_freeXmlNode(child);
        child = next;
    }

    xapi_xmlFreeAttributes(node->attributes);
    free(node->name);
    free(node->text);
    free(node);
}

void xapi_freeXmlTree(XXmlTree *tree)
{
    if (!tree) { return; }
    xapi_freeXmlNode(tree->root);
    tree->root = NULL;
}

static int xapi_xmlSkipComment(XXmlParser *parser)
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

static int xapi_xmlSkipInstruction(XXmlParser *parser)
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

static int xapi_xmlSkipDeclaration(XXmlParser *parser)
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

static int xapi_xmlSkipMisc(XXmlParser *parser)
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

static int xapi_xmlParseTextNode(XXmlParser *parser, XXmlNode *parent)
{
    const char *begin = parser->cursor;
    const char *end   = begin;

    while (*end && *end != '<')
    {
        end++;
    }

    parser->cursor = end;
    if (begin == end || xapi_xmlIsAllSpaces(begin, end)) { return XAPI_XML_PARSE_OK; }

    XXmlNode *textNode = xapi_xmlCreateNode(XAPI_XML_NODE_TEXT);
    if (!textNode) { return XAPI_XML_PARSE_NO_MEMORY; }

    textNode->text = xapi_xmlDupRange(begin, end);
    if (!textNode->text)
    {
        xapi_freeXmlNode(textNode);
        return XAPI_XML_PARSE_NO_MEMORY;
    }

    return xapi_xmlAppendChild(parent, textNode);
}

static int xapi_xmlParseCDataNode(XXmlParser *parser, XXmlNode *parent)
{
    const char *begin = parser->cursor + 9;
    const char *p     = begin;

    while (*p)
    {
        if (p[0] == ']' && p[1] == ']' && p[2] == '>')
        {
            XXmlNode *textNode = xapi_xmlCreateNode(XAPI_XML_NODE_TEXT);
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

static int xapi_xmlParseName(XXmlParser *parser, char **outName)
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

static int xapi_xmlParseAttribute(XXmlParser *parser, XXmlNode *node)
{
    XXmlAttribute *attr = (XXmlAttribute *)calloc(1, sizeof(XXmlAttribute));
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
        XXmlAttribute *tail = node->attributes;
        while (tail->next)
        {
            tail = tail->next;
        }
        tail->next = attr;
    }

    return XAPI_XML_PARSE_OK;
}

static int xapi_xmlParseElement(XXmlParser *parser, XXmlNode **outNode)
{
    if (*parser->cursor != '<') { return XAPI_XML_PARSE_SYNTAX_ERROR; }
    parser->cursor++;

    XXmlNode *node = xapi_xmlCreateNode(XAPI_XML_NODE_ELEMENT);
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
            XXmlNode *child = NULL;
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

int xapi_parseXml(const char *xmlText, XXmlTree *outTree)
{
    if (!xmlText || !outTree) { return XAPI_XML_PARSE_INVALID_PARAM; }

    outTree->root = NULL;

    XXmlParser parser;
    parser.cursor = xmlText;

    int ret = xapi_xmlSkipMisc(&parser);
    if (ret != XAPI_XML_PARSE_OK) { return ret; }

    XXmlNode *root = NULL;
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
