#include "./xapi/include/x3api.h"
#include <xj380_i18n.h>

#define XMLV_WINDOW_WIDTH   980
#define XMLV_WINDOW_HEIGHT  620

#define XMLV_PADDING        8
#define XMLV_LINE_HEIGHT    16

#define XMLV_INPUT_MAX      4096
#define XMLV_STATUS_MAX     128

#define XMLV_STATE_MAX      2048
#define XMLV_LINES_MAX      2048
#define XMLV_LINE_TEXT_MAX  384

#define XMLV_HEADER_Y1      0
#define XMLV_HEADER_Y2      24

#define XMLV_INPUT_Y1       30
#define XMLV_INPUT_Y2       56

#define XMLV_STATUS_Y1      60
#define XMLV_STATUS_Y2      80

#define XMLV_TREE_Y1        86
#define XMLV_TREE_Y2        (XMLV_WINDOW_HEIGHT - 8)

#define XMLV_BTN_W          72
#define XMLV_BTN_GAP        6

typedef struct xmlv_NodeState
{
    const xapi_XmlNode *node;
    bool                expanded;
} xmlv_NodeState;

typedef struct xmlv_Line
{
    const xapi_XmlNode *node;
    bool                canToggle;
    char                text[XMLV_LINE_TEXT_MAX];
} xmlv_Line;

static HDLE         g_handle = 0;
static bool         g_needRedraw = true;
static bool         g_needExit = false;
static bool         g_inputFocus = true;

static char         g_input[XMLV_INPUT_MAX];
static int          g_inputLen = 0;
static char         g_status[XMLV_STATUS_MAX];

static xapi_XmlTree g_tree;
static bool         g_treeValid = false;

static xmlv_NodeState g_states[XMLV_STATE_MAX];
static int            g_stateCount = 0;

static xmlv_Line      g_lines[XMLV_LINES_MAX];
static int            g_lineCount = 0;
static int            g_scrollTop = 0;
static UINT64         g_width = XMLV_WINDOW_WIDTH;
static UINT64         g_height = XMLV_WINDOW_HEIGHT;
static int            g_language = XJ380_LANGUAGE_ZH_CN;

static const char *g_defaultXml =
    "<node1><node2 attribute=\"3\"></node2><node3><node4>something</node4><node4>something</node4></node3></node1>";

static char *xmlv_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static int xmlv_min(int a, int b)
{
    return a < b ? a : b;
}

static int xmlv_appendChar(char *dst, int pos, int limit, char ch)
{
    if (pos >= limit - 1) { return pos; }
    dst[pos++] = ch;
    dst[pos]   = '\0';
    return pos;
}

static int xmlv_appendString(char *dst, int pos, int limit, const char *text)
{
    if (!text) { text = ""; }
    while (*text && pos < limit - 1)
    {
        dst[pos++] = *text++;
    }
    dst[pos] = '\0';
    return pos;
}

static void xmlv_setStatus(const char *text)
{
    if (!text) { text = ""; }
    strncpy(g_status, text, XMLV_STATUS_MAX - 1);
    g_status[XMLV_STATUS_MAX - 1] = '\0';
}

static int xmlv_inputX1()
{
    return XMLV_PADDING;
}

static int xmlv_inputX2()
{
    return (int)g_width - XMLV_PADDING - XMLV_BTN_W * 2 - XMLV_BTN_GAP * 2;
}

static int xmlv_parseBtnX1()
{
    return xmlv_inputX2() + XMLV_BTN_GAP;
}

static int xmlv_parseBtnX2()
{
    return xmlv_parseBtnX1() + XMLV_BTN_W;
}

static int xmlv_clearBtnX1()
{
    return xmlv_parseBtnX2() + XMLV_BTN_GAP;
}

static int xmlv_clearBtnX2()
{
    return xmlv_clearBtnX1() + XMLV_BTN_W;
}

static int xmlv_treeInnerX1()
{
    return XMLV_PADDING + 2;
}

static int xmlv_treeInnerX2()
{
    return (int)g_width - XMLV_PADDING - 2;
}

static int xmlv_treeContentY1()
{
    return XMLV_TREE_Y1 + 6;
}

static int xmlv_treeContentY2()
{
    return (int)g_height - XMLV_PADDING - 4;
}

static int xmlv_scrollUpX1()
{
    return xmlv_treeInnerX2() - 40;
}

static int xmlv_scrollUpX2()
{
    return xmlv_treeInnerX2() - 22;
}

static int xmlv_scrollDownX1()
{
    return xmlv_treeInnerX2() - 20;
}

static int xmlv_scrollDownX2()
{
    return xmlv_treeInnerX2() - 2;
}

static int xmlv_scrollBtnY1()
{
    return XMLV_TREE_Y1 + 2;
}

static int xmlv_scrollBtnY2()
{
    return XMLV_TREE_Y1 + 18;
}

static int xmlv_visibleRows()
{
    int height = xmlv_treeContentY2() - xmlv_treeContentY1() + 1;
    if (height <= 0) { return 0; }
    return height / XMLV_LINE_HEIGHT;
}

static int xmlv_maxScroll()
{
    int rows = xmlv_visibleRows();
    if (g_lineCount <= rows) { return 0; }
    return g_lineCount - rows;
}

static void xmlv_fixScroll()
{
    int maxTop = xmlv_maxScroll();
    if (g_scrollTop < 0) { g_scrollTop = 0; }
    if (g_scrollTop > maxTop) { g_scrollTop = maxTop; }
}

static void xmlv_scrollBy(int delta)
{
    g_scrollTop += delta;
    xmlv_fixScroll();
}

static void xmlv_freeTree()
{
    if (g_treeValid)
    {
        xapi_freeXmlTree(&g_tree);
        g_treeValid = false;
    }
}

static int xmlv_stateIndex(const xapi_XmlNode *node, bool createIfMissing)
{
    for (int i = 0; i < g_stateCount; i++)
    {
        if (g_states[i].node == node) { return i; }
    }

    if (!createIfMissing || g_stateCount >= XMLV_STATE_MAX) { return -1; }

    g_states[g_stateCount].node     = node;
    g_states[g_stateCount].expanded = true;
    g_stateCount++;
    return g_stateCount - 1;
}

static bool xmlv_isExpanded(const xapi_XmlNode *node)
{
    int idx = xmlv_stateIndex(node, true);
    if (idx < 0) { return true; }
    return g_states[idx].expanded;
}

static void xmlv_toggleExpanded(const xapi_XmlNode *node)
{
    int idx = xmlv_stateIndex(node, true);
    if (idx < 0) { return; }
    g_states[idx].expanded = !g_states[idx].expanded;
}

static bool xmlv_linePush(const xapi_XmlNode *node, bool canToggle, const char *text)
{
    if (g_lineCount >= XMLV_LINES_MAX) { return false; }

    g_lines[g_lineCount].node      = node;
    g_lines[g_lineCount].canToggle = canToggle;
    strncpy(g_lines[g_lineCount].text, text, XMLV_LINE_TEXT_MAX - 1);
    g_lines[g_lineCount].text[XMLV_LINE_TEXT_MAX - 1] = '\0';
    g_lineCount++;
    return true;
}

static void xmlv_formatElementLine(const xapi_XmlNode *node, int depth, bool canToggle, bool expanded, char *out)
{
    int pos = 0;
    out[0]  = '\0';

    for (int i = 0; i < depth; i++)
    {
        pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, "  ");
    }

    if (canToggle)
    {
        pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, expanded ? "[-] " : "[+] ");
    }
    else
    {
        pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, "    ");
    }

    pos = xmlv_appendChar(out, pos, XMLV_LINE_TEXT_MAX, '<');
    pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, node->name ? node->name : "");

    const xapi_XmlAttribute *attr = node->attributes;
    while (attr)
    {
        pos = xmlv_appendChar(out, pos, XMLV_LINE_TEXT_MAX, ' ');
        pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, attr->name ? attr->name : "");
        pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, "=\"");
        pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, attr->value ? attr->value : "");
        pos = xmlv_appendChar(out, pos, XMLV_LINE_TEXT_MAX, '"');
        attr = attr->next;
    }

    xmlv_appendChar(out, pos, XMLV_LINE_TEXT_MAX, '>');
}

static void xmlv_formatTextLine(const xapi_XmlNode *node, int depth, char *out)
{
    int pos = 0;
    out[0]  = '\0';

    for (int i = 0; i < depth; i++)
    {
        pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, "  ");
    }
    pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, "    \"");
    pos = xmlv_appendString(out, pos, XMLV_LINE_TEXT_MAX, node->text ? node->text : "");
    xmlv_appendChar(out, pos, XMLV_LINE_TEXT_MAX, '"');
}

static void xmlv_buildLinesFromNode(const xapi_XmlNode *node, int depth)
{
    const xapi_XmlNode *current = node;

    while (current && g_lineCount < XMLV_LINES_MAX)
    {
        if (current->type == XAPI_XML_NODE_ELEMENT)
        {
            bool canToggle = (current->firstChild != NULL);
            bool expanded  = xmlv_isExpanded(current);

            char line[XMLV_LINE_TEXT_MAX];
            xmlv_formatElementLine(current, depth, canToggle, expanded, line);
            if (!xmlv_linePush(current, canToggle, line)) { return; }

            if (canToggle && expanded)
            {
                xmlv_buildLinesFromNode(current->firstChild, depth + 1);
            }
        }
        else if (current->type == XAPI_XML_NODE_TEXT)
        {
            char line[XMLV_LINE_TEXT_MAX];
            xmlv_formatTextLine(current, depth, line);
            if (!xmlv_linePush(current, false, line)) { return; }
        }

        current = current->nextSibling;
    }
}

static void xmlv_rebuildLines()
{
    g_lineCount = 0;
    if (g_treeValid && g_tree.root)
    {
        xmlv_buildLinesFromNode(g_tree.root, 0);
    }
    xmlv_fixScroll();
}

static void xmlv_parseInput()
{
    xmlv_freeTree();
    g_stateCount = 0;
    g_lineCount  = 0;
    g_scrollTop  = 0;

    if (g_inputLen == 0)
    {
        xmlv_setStatus(xmlv_tr("输入为空", "Input is empty"));
        g_needRedraw = true;
        return;
    }

    int ret = xapi_parseXml(g_input, &g_tree);
    if (ret == XAPI_XML_PARSE_OK)
    {
        g_treeValid = true;
        xmlv_rebuildLines();
        snprintf(g_status, XMLV_STATUS_MAX, xmlv_tr("解析成功，行数=%d", "Parse OK, lines=%d"), g_lineCount);
    }
    else
    {
        g_treeValid = false;
        snprintf(g_status, XMLV_STATUS_MAX, xmlv_tr("解析失败，代码=%d", "Parse failed, code=%d"), ret);
    }
    g_needRedraw = true;
}

static bool xmlv_isPrintable(char ch)
{
    return ch >= 32 && ch <= 126;
}

static void xmlv_inputAppend(char ch)
{
    if (g_inputLen >= XMLV_INPUT_MAX - 1) { return; }
    g_input[g_inputLen++] = ch;
    g_input[g_inputLen]   = '\0';
    g_needRedraw          = true;
}

static void xmlv_inputBackspace()
{
    if (g_inputLen <= 0) { return; }
    g_inputLen--;
    g_input[g_inputLen] = '\0';
    g_needRedraw        = true;
}

static void xmlv_clearInput()
{
    g_inputLen = 0;
    g_input[0] = '\0';
    xmlv_setStatus(xmlv_tr("输入已清空", "Input cleared"));
    g_needRedraw = true;
}

static void xmlv_setDefaultInput()
{
    strncpy(g_input, g_defaultXml, XMLV_INPUT_MAX - 1);
    g_input[XMLV_INPUT_MAX - 1] = '\0';
    g_inputLen = (int)strlen(g_input);
}

static void xmlv_getInputTail(char *out, int outLen)
{
    int visibleChars = (xmlv_inputX2() - xmlv_inputX1() - 12) / 9;
    if (visibleChars < 1) { visibleChars = 1; }

    int start = 0;
    if (g_inputLen > visibleChars)
    {
        start = g_inputLen - visibleChars;
    }

    strncpy(out, g_input + start, outLen - 1);
    out[outLen - 1] = '\0';
}

static void xmlv_drawUi()
{
    g_language = xj380_read_language();
    xapi_GetWindowSize(g_handle, &g_width, &g_height);
    xmlv_fixScroll();
    xapi_DrawRect(g_handle, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, 0xf6f8faff, true);

    xapi_DrawRect(g_handle, 0, XMLV_HEADER_Y1, (UINT32)g_width - 1, XMLV_HEADER_Y2, 0xe4e9efff, true);
    xapi_DrawText(g_handle, XMLV_PADDING, 2, xmlv_tr("XML 浏览器", "XML Viewer"), 11, 0x0a1929ff);
    xapi_DrawText(g_handle, 150, 2,
                  xmlv_tr("输入 XML 后按 Enter 或解析。点击 [+]/[-] 行可折叠。",
                           "Enter XML, then press Enter or Parse. Click [+]/[-] to fold."),
                  10,
                  0x2b3a49ff);

    xapi_DrawRect(g_handle, xmlv_inputX1(), XMLV_INPUT_Y1, xmlv_inputX2(), XMLV_INPUT_Y2, 0x00000020, false);
    xapi_DrawRect(g_handle, xmlv_inputX1() + 1, XMLV_INPUT_Y1 + 1, xmlv_inputX2() - 1, XMLV_INPUT_Y2 - 1, 0xffffffff,
                  true);

    xapi_DrawRect(g_handle, xmlv_parseBtnX1(), XMLV_INPUT_Y1, xmlv_parseBtnX2(), XMLV_INPUT_Y2, 0x2d8cf0ff, true);
    xapi_DrawText(g_handle, xmlv_parseBtnX1() + 15, XMLV_INPUT_Y1 + 2, xmlv_tr("解析", "Parse"), 11, 0xffffffff);

    xapi_DrawRect(g_handle, xmlv_clearBtnX1(), XMLV_INPUT_Y1, xmlv_clearBtnX2(), XMLV_INPUT_Y2, 0x6c757dff, true);
    xapi_DrawText(g_handle, xmlv_clearBtnX1() + 15, XMLV_INPUT_Y1 + 2, xmlv_tr("清空", "Clear"), 11, 0xffffffff);

    xapi_DrawRect(g_handle, XMLV_PADDING, XMLV_STATUS_Y1, (UINT32)g_width - XMLV_PADDING, XMLV_STATUS_Y2, 0xffffffff,
                  true);
    xapi_DrawText(g_handle, XMLV_PADDING + 4, XMLV_STATUS_Y1 + 1, g_status, 10, 0x203040ff);

    char inputTail[XMLV_INPUT_MAX];
    xmlv_getInputTail(inputTail, sizeof(inputTail));
    xapi_DrawSWText(g_handle, xmlv_inputX1() + 5, XMLV_INPUT_Y1 + 6, inputTail, 0x101820ff);

    if (g_inputFocus)
    {
        int tailLen = (int)strlen(inputTail);
        int cx      = xmlv_inputX1() + 5 + tailLen * 9;
        if (cx > xmlv_inputX2() - 3) { cx = xmlv_inputX2() - 3; }
        xapi_DrawLine(g_handle, cx, XMLV_INPUT_Y1 + 5, cx, XMLV_INPUT_Y2 - 4, 0x2d8cf0ff);
    }

    xapi_DrawRect(g_handle, XMLV_PADDING, XMLV_TREE_Y1, (UINT32)g_width - XMLV_PADDING, (UINT32)g_height - XMLV_PADDING, 0x00000020, false);
    xapi_DrawRect(g_handle, XMLV_PADDING + 1, XMLV_TREE_Y1 + 1, (UINT32)g_width - XMLV_PADDING - 1, (UINT32)g_height - XMLV_PADDING - 1,
                  0xffffffff, true);

    xapi_DrawRect(g_handle, xmlv_scrollUpX1(), xmlv_scrollBtnY1(), xmlv_scrollUpX2(), xmlv_scrollBtnY2(), 0xe4e9efff, true);
    xapi_DrawRect(g_handle, xmlv_scrollDownX1(), xmlv_scrollBtnY1(), xmlv_scrollDownX2(), xmlv_scrollBtnY2(), 0xe4e9efff, true);
    xapi_DrawText(g_handle, xmlv_scrollUpX1() + 6, xmlv_scrollBtnY1() - 1, (char *)"<", 10, 0x243444ff);
    xapi_DrawText(g_handle, xmlv_scrollDownX1() + 6, xmlv_scrollBtnY1() - 1, (char *)">", 10, 0x243444ff);

    int rows  = xmlv_visibleRows();
    int start = g_scrollTop;
    int end   = xmlv_min(g_lineCount, start + rows);

    for (int i = start; i < end; i++)
    {
        int y = xmlv_treeContentY1() + (i - start) * XMLV_LINE_HEIGHT;
        xapi_DrawSWText(g_handle, xmlv_treeInnerX1() + 2, y, g_lines[i].text, g_lines[i].canToggle ? 0x1a4f8bff : 0x152433ff);
    }

    xapi_RefreshWindow(g_handle);
}

static void xmlv_onTreeClick(int x, int y)
{
    (void)x;

    int row = (y - xmlv_treeContentY1()) / XMLV_LINE_HEIGHT;
    int rows = xmlv_visibleRows();
    if (row < 0 || row >= rows) { return; }

    int idx = g_scrollTop + row;
    if (idx < 0 || idx >= g_lineCount) { return; }

    if (g_lines[idx].canToggle && g_lines[idx].node)
    {
        xmlv_toggleExpanded(g_lines[idx].node);
        xmlv_rebuildLines();
        g_needRedraw = true;
    }
}

static bool xmlv_inRect(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

static void xmlv_handleLeftClick(int x, int y)
{
    if (xmlv_inRect(x, y, xmlv_parseBtnX1(), XMLV_INPUT_Y1, xmlv_parseBtnX2(), XMLV_INPUT_Y2))
    {
        g_inputFocus = true;
        xmlv_parseInput();
        return;
    }

    if (xmlv_inRect(x, y, xmlv_clearBtnX1(), XMLV_INPUT_Y1, xmlv_clearBtnX2(), XMLV_INPUT_Y2))
    {
        g_inputFocus = true;
        xmlv_clearInput();
        return;
    }

    if (xmlv_inRect(x, y, xmlv_scrollUpX1(), xmlv_scrollBtnY1(), xmlv_scrollUpX2(), xmlv_scrollBtnY2()))
    {
        xmlv_scrollBy(-1);
        g_needRedraw = true;
        return;
    }

    if (xmlv_inRect(x, y, xmlv_scrollDownX1(), xmlv_scrollBtnY1(), xmlv_scrollDownX2(), xmlv_scrollBtnY2()))
    {
        xmlv_scrollBy(1);
        g_needRedraw = true;
        return;
    }

    if (xmlv_inRect(x, y, xmlv_inputX1(), XMLV_INPUT_Y1, xmlv_inputX2(), XMLV_INPUT_Y2))
    {
        g_inputFocus = true;
        g_needRedraw = true;
        return;
    }

    if (xmlv_inRect(x, y, XMLV_PADDING, XMLV_TREE_Y1, (int)g_width - XMLV_PADDING, (int)g_height - XMLV_PADDING))
    {
        g_inputFocus = false;
        xmlv_onTreeClick(x, y);
        g_needRedraw = true;
        return;
    }
}

static void xmlv_handleChar(char ch)
{
    if (!g_inputFocus)
    {
        if (ch == 'w' || ch == 'W')
        {
            xmlv_scrollBy(-1);
            g_needRedraw = true;
            return;
        }
        if (ch == 's' || ch == 'S')
        {
            xmlv_scrollBy(1);
            g_needRedraw = true;
            return;
        }
        return;
    }

    if (xmlv_isPrintable(ch))
    {
        xmlv_inputAppend(ch);
    }
}

static void xmlv_handleSpecialChar(char ch)
{
    if (ch == '\n')
    {
        xmlv_parseInput();
        return;
    }

    if (ch == '\b' && g_inputFocus)
    {
        xmlv_inputBackspace();
    }
}

void xmlv_messagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    switch (Type)
    {
    case MSG_LBUTTON:
        xmlv_handleLeftClick((int)hData, (int)lData);
        break;
    case MSG_CHAR:
        xmlv_handleChar((char)lData);
        break;
    case MSG_SPCHAR:
        xmlv_handleSpecialChar((char)lData);
        break;
    case MSG_ROLLER:
        if ((int)lData > 0)
            xmlv_scrollBy(-1);
        else if ((int)lData < 0)
            xmlv_scrollBy(1);
        g_needRedraw = true;
        break;
    case MSG_RESIZE:
        g_width = hData;
        g_height = lData;
        xmlv_fixScroll();
        g_needRedraw = true;
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    XWINDOW win;
    g_language = xj380_read_language();
    win.title  = xmlv_tr("XML 浏览器", "XML Viewer");
    win.width  = XMLV_WINDOW_WIDTH;
    win.height = XMLV_WINDOW_HEIGHT;
    win.sets   = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;

    xapi_CreateWindow(&g_handle, &win);
    xapi_SetIcon(g_handle, "/system/icon/texter.png");
    SetMsgPrcor(g_handle, xmlv_messagePrcor);

    xmlv_setDefaultInput();
    xmlv_setStatus(xmlv_tr("就绪", "Ready"));
    xmlv_parseInput();
    g_needRedraw = true;

    while (!g_needExit)
    {
        if (g_needRedraw)
        {
            g_needRedraw = false;
            xmlv_drawUi();
        }
        __asm__ __volatile__("pause");
    }

    xmlv_freeTree();
    xapi_CloseWindow(g_handle);
    return 0;
}
