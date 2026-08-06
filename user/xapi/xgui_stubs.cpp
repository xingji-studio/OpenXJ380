#include "xguiapi.h"

extern "C" {

void xapi_CreateWindow(HDLE *handle, XWINDOW *xwin)
{
    (void)xwin;
    if (handle != NULL) *handle = 0;
}

void xapi_SetWindowTitle(HDLE handle, WSTR str) { (void)handle; (void)str; }
void xapi_CloseWindow(HDLE handle) { (void)handle; }
void xapi_SetIcon(HDLE handle, WSTR path) { (void)handle; (void)path; }

void xapi_GetWindowSize(HDLE handle, UINT64 *width, UINT64 *height)
{
    (void)handle;
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
}

void xapi_DrawPoint(HDLE handle, UINT32 x, UINT32 y, UINT32 color)
{
    (void)handle; (void)x; (void)y; (void)color;
}

void xapi_DrawLine(HDLE handle, UINT32 x1, UINT32 y1, UINT32 x2, UINT32 y2, UINT32 color)
{
    (void)handle; (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
}

void xapi_DrawRect(HDLE handle, UINT32 x1, UINT32 y1, UINT32 x2, UINT32 y2, UINT32 color, bool fill)
{
    (void)handle; (void)x1; (void)y1; (void)x2; (void)y2; (void)color; (void)fill;
}

INT32 xapi_DrawSvg(HDLE handle, UINT32 x, UINT32 y, UINT32 width, WSTR svg_text, bool enable_trans)
{
    (void)handle; (void)x; (void)y; (void)width; (void)svg_text; (void)enable_trans;
    return -1;
}

INT32 xapi_DrawFA(HDLE handle, UINT32 x, UINT32 y, UINT32 width, char *name, bool enable_trans)
{
    (void)handle; (void)x; (void)y; (void)width; (void)name; (void)enable_trans;
    return -1;
}

void xapi_DrawText(HDLE handle, UINT32 x, UINT32 y, WSTR str, UINT32 size, UINT32 color)
{
    (void)handle; (void)x; (void)y; (void)str; (void)size; (void)color;
}

void xapi_DrawTextl(HDLE handle, UINT32 x, UINT32 y, WSTR str, UINT32 size, UINT32 color, UINT32 *width)
{
    (void)handle; (void)x; (void)y; (void)str; (void)size; (void)color;
    if (width != NULL) *width = 0;
}

void xapi_DrawSWText(HDLE handle, UINT32 x, UINT32 y, WSTR str, UINT32 color)
{
    (void)handle; (void)x; (void)y; (void)str; (void)color;
}

UINT64 xapi_CalcTextWidth(WSTR str, UINT32 size)
{
    (void)str; (void)size;
    return 0;
}

void xapi_DrawBMP(HDLE handle, UINT32 x, UINT32 y, UINT32 width, UINT32 height, WSTR path)
{
    (void)handle; (void)x; (void)y; (void)width; (void)height; (void)path;
}

void xapi_DrawPNG(HDLE handle, UINT32 x, UINT32 y, UINT32 width, UINT32 height, WSTR path)
{
    (void)handle; (void)x; (void)y; (void)width; (void)height; (void)path;
}

void xapi_DrawPicture(HDLE handle, UINT32 x, UINT32 y, UINT32 width, UINT32 height, WSTR path)
{
    (void)handle; (void)x; (void)y; (void)width; (void)height; (void)path;
}

bool xapi_LoadPicture(XCOLORA *buffer, UINT32 width, UINT32 height, WSTR path)
{
    (void)buffer; (void)width; (void)height; (void)path;
    return false;
}

void xapi_GetPicSize(UINT32 *width, UINT32 *height, WSTR path)
{
    (void)path;
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
}

void SetMsgPrcor(HDLE handle, MsgPrcor func) { (void)handle; (void)func; }

void xapi_ReadBuffer(HDLE handle, UINT32 x, UINT32 y, UINT32 width, UINT32 height, XCOLOR *buffer)
{
    (void)handle; (void)x; (void)y; (void)width; (void)height; (void)buffer;
}

void xapi_WriteBuffer(HDLE handle, UINT32 x, UINT32 y, UINT32 width, UINT32 height, XCOLOR *buffer)
{
    (void)handle; (void)x; (void)y; (void)width; (void)height; (void)buffer;
}

void xapi_ReadBufferA(HDLE handle, UINT32 x, UINT32 y, UINT32 width, UINT32 height, XCOLOR *buffer)
{
    (void)handle; (void)x; (void)y; (void)width; (void)height; (void)buffer;
}

void xapi_WriteBufferA(HDLE handle, UINT32 x, UINT32 y, UINT32 width, UINT32 height, XCOLORA *buffer)
{
    (void)handle; (void)x; (void)y; (void)width; (void)height; (void)buffer;
}

void xapi_RefreshWindow(HDLE handle) { (void)handle; }
void xapi_RefreshPartWindow(HDLE handle, UINT32 x1, UINT32 y1, UINT32 x2, UINT32 y2)
{
    (void)handle; (void)x1; (void)y1; (void)x2; (void)y2;
}

void xapi_Button(HDLE handle, UINT64 id, UINT64 x, UINT64 y, WSTR text)
{
    (void)handle; (void)id; (void)x; (void)y; (void)text;
}

void xapi_ButtonEmp(HDLE handle, UINT64 id, UINT64 x, UINT64 y, WSTR text)
{
    (void)handle; (void)id; (void)x; (void)y; (void)text;
}

void xapi_DeleteButton(HDLE handle, UINT64 id) { (void)handle; (void)id; }
void xapi_PutSwitch(HDLE handle, UINT64 x, UINT64 y, UINT64 status, UINT64 id)
{
    (void)handle; (void)x; (void)y; (void)status; (void)id;
}

void xapi_SetSwitch(HDLE handle, UINT64 id, UINT64 status) { (void)handle; (void)id; (void)status; }
void xapi_DeleteSwitch(HDLE handle, UINT64 id) { (void)handle; (void)id; }
void xapi_PutVerticalScrollBar(HDLE handle, UINT64 x, UINT64 y, UINT64 length, UINT64 thumb_length, UINT64 id)
{
    (void)handle; (void)x; (void)y; (void)length; (void)thumb_length; (void)id;
}

void xapi_PutHorizontalScrollBar(HDLE handle, UINT64 x, UINT64 y, UINT64 length, UINT64 thumb_length, UINT64 id)
{
    (void)handle; (void)x; (void)y; (void)length; (void)thumb_length; (void)id;
}

void xapi_DeleteScrollBar(HDLE handle, UINT64 id) { (void)handle; (void)id; }
void xapi_SetScrollBarPosition(HDLE handle, UINT64 id, UINT64 position)
{
    (void)handle; (void)id; (void)position;
}

UINT64 xapi_PutTextInputBox(HDLE handle, UINT64 x, UINT64 y, UINT64 width, WSTR text)
{
    (void)handle; (void)x; (void)y; (void)width; (void)text;
    return 0;
}

void xapi_GetTextInputBox(UINT64 id, WSTR text) { (void)id; (void)text; }
void xapi_DeleteTextInputBox(UINT64 id) { (void)id; }
void xapi_RegisterRightButtonMenu(HDLE handle, RightMenuItem *items, UINT64 count)
{
    (void)handle; (void)items; (void)count;
}

void xapi_DeleteRightButtonMenu(HDLE handle) { (void)handle; }

}
