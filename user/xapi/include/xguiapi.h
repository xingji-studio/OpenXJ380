/*
 *
 *      XINGJI XJ380 Application Compile Tools (XACT) 
 *      XACT C\C++ Header
 *      XJ380API XAPI Edition GUI Part
 *      Release Version 1.2.0 - 2026/1/16
 *      Copyright(C) XINGJI Interactive Software 2017 - 2026 All rights reserved.
 * 
 *      A XINGJI Interactive Software Production
 * 
 */

#pragma once

#include "stdint.h"
#include "./liballoc/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef UINT64 HDLE;
typedef void (*MsgPrcor)(UINT64 Type, UINT64 hData, UINT64 lData);

typedef struct XWINDOW
{
    UINT32 width;
    UINT32 height;
    WSTR   title;
    UINT8  sets;
} XWINDOW;

#pragma pack(1)

typedef struct XCOLOR
{
    UINT8 Red;
    UINT8 Green;
    UINT8 Blue;
} XCOLOR;

#pragma pack()

typedef struct XCOLORA
{
    UINT8 Red;
    UINT8 Green;
    UINT8 Blue;
    UINT8 Alpha;
} XCOLORA;

typedef struct
{
	UINT64 	CRLid;
	WSTR	text;
} RightMenuItem;

#define XAPI_TEXT_INPUT_BOX_BUFFER_SIZE 256


// 4-1
#define XWIN_NORMAL				0
#define XWIN_FRAME_OFF			1
#define XWIN_FULL_SCR			2
#define XWIN_DESKTOP            3
#define XWIN_DOCK               4
#define XWIN_LOGIN              5
#define XWIN_TYPE_MASK         0x0f
#define XWIN_SUPPORT_RESIZEABLE  0x80

void  xapi_CreateWindow(
	HDLE*		handle,
	XWINDOW* 	xwin
);

void  xapi_SetWindowTitle(
	HDLE  	handle,
	WSTR  	str
);

void  xapi_CloseWindow(
    HDLE  	handle
);

void  xapi_SetIcon(
	HDLE  	handle,
	WSTR  	path
);

void xapi_GetWindowSize(
	HDLE handle, 
	UINT64* width, 
	UINT64* height
);


// 4-2
void  xapi_DrawPoint (
	HDLE  	handle,
	UINT32  x,
	UINT32  y,
	UINT32  color
);

void  xapi_DrawLine(
    HDLE  	handle,
	UINT32 	x1,
	UINT32 	y1,
	UINT32 	x2,
	UINT32 	y2,
	UINT32 	color
);

void  xapi_DrawRect(
	HDLE 	handle,
	UINT32 	x1,
	UINT32  y1,
	UINT32  x2,
	UINT32 	y2,
	UINT32 	color,
	bool  	fill
);

INT32 xapi_DrawSvg(
	HDLE  	handle,
	UINT32  x,
	UINT32  y,
	UINT32  width,
	WSTR    svgText,
	bool    enableTrans
);

INT32 xapi_DrawFA(
	HDLE  	handle,
	UINT32  x,
	UINT32  y,
	UINT32  width,
	char   *name,
	bool    enableTrans
);

// void  xapi_DrawCircle(
// 	HDLE  	handle,
// 	UINT32 	x,
// 	UINT32 	y,
// 	UINT32 	radius,
// 	UINT32 	color,
// 	bool  	fill
// );

void  xapi_DrawText(
	HDLE  	handle,
	UINT32 	x,
	UINT32 	y,
	WSTR 	str,
	UINT32	size,
	UINT32  color
);

void  xapi_DrawTextl(
	HDLE  	handle,
	UINT32 	x,
	UINT32 	y,
	WSTR 	str,
	UINT32	size,
	UINT32  color,
	UINT32 *width
);

void xapi_DrawSWText(
	HDLE  	handle,
	UINT32 	x,
	UINT32 	y,
	WSTR 	str,
	UINT32  color
);

UINT64 xapi_CalcTextWidth(WSTR str, UINT32 size);

// 4-3
void  xapi_DrawBMP (
	HDLE  	handle,
	UINT32  x,
	UINT32  y,
	UINT32	width,
	UINT32	height,
	WSTR  	path
);

void  xapi_DrawPNG(
	HDLE  	handle,
	UINT32  x,
	UINT32  y,
	UINT32	width,
	UINT32	height,
	WSTR  	path
);

void xapi_DrawPicture(
	HDLE  	handle,
	UINT32  x,
	UINT32  y,
	UINT32	width,
	UINT32	height,
	WSTR  	path
);

bool xapi_LoadPicture(
	XCOLORA *buffer,
	UINT32   width,
	UINT32   height,
	WSTR     path
);

void xapi_GetPicSize(
	UINT32	*width,
	UINT32	*height,
	WSTR  	 path
);

// 4-4
#include "libsys.h"
void SetMsgPrcor(HDLE handle, MsgPrcor func);

enum XAPI_SPECIAL_KEY
{
    XKEY_ESC = 128,
    XKEY_BACKSPACE,
    XKEY_TAB,
    XKEY_ENTER,
    XKEY_CAPS,
    XKEY_SHIFT,
    XKEY_CTRL,
    XKEY_ALT,
    XKEY_SPACE,
    XKEY_F1,
    XKEY_F2,
    XKEY_F3,
    XKEY_F4,
    XKEY_F5,
    XKEY_F6,
    XKEY_F7,
    XKEY_F8,
    XKEY_F9,
    XKEY_F10,
    XKEY_F11,
    XKEY_F12,
    XKEY_NUML,
    XKEY_SCROLL,
    XKEY_HOME,
    XKEY_UP,
    XKEY_PAGE_UP,
    XKEY_LEFT,
    XKEY_RIGHT,
    XKEY_END,
    XKEY_DOWN,
    XKEY_PAGE_DOWN,
    XKEY_INSERT,
    XKEY_DELETE
};

// 4-5
void  xapi_ReadBuffer(
	HDLE  	 	handle,
	UINT32  	x,
	UINT32  	y,
	UINT32  	width,
	UINT32		height,
	XCOLOR*  	buffer
);

void xapi_WriteBuffer(
	HDLE  	 	handle,
	UINT32  	x,
	UINT32  	y,
	UINT32  	width,
	UINT32  	height,
	XCOLOR*  	buffer
);

void xapi_ReadBufferA(
	HDLE  	 	handle,
	UINT32  	x,
	UINT32  	y,
	UINT32  	width,
	UINT32		height,
	XCOLOR*  	buffer
);

void xapi_WriteBufferA(
	HDLE  	 	handle,
	UINT32  	x,
	UINT32  	y,
	UINT32  	width,
	UINT32  	height,
	XCOLORA*  	buffer
);

void xapi_RefreshWindow(
	HDLE		handle
);

void xapi_RefreshPartWindow(
	HDLE 	handle,
	UINT32  x1,
	UINT32  y1,
	UINT32  x2,
	UINT32  y2
);

// 4-6
void xapi_Button(
	HDLE  	handle,
	UINT64  CRLid,
	UINT64  x,
	UINT64  y,
	WSTR  	text
);

void xapi_ButtonEmp(
	HDLE  	handle,
	UINT64  CRLid,
	UINT64  x,
	UINT64  y,
	WSTR  	text
);

void xapi_DeleteButton(
	HDLE  	handle,
	UINT64  CRLid
);

void xapi_PutSwitch(
	HDLE  	handle,
	UINT64  x,
	UINT64  y,
	UINT64  status,
	UINT64  CRLid
);

void xapi_SetSwitch(
	HDLE  	handle,
	UINT64  CRLid,
	UINT64  status
);

void xapi_DeleteSwitch(
	HDLE  	handle,
	UINT64  CRLid
);

void xapi_PutVerticalScrollBar(
	HDLE  	handle,
	UINT64  x,
	UINT64  y,
	UINT64  length,
	UINT64  thumbLength,
	UINT64  CRLid
);

void xapi_PutHorizontalScrollBar(
	HDLE  	handle,
	UINT64  x,
	UINT64  y,
	UINT64  length,
	UINT64  thumbLength,
	UINT64  CRLid
);

void xapi_DeleteScrollBar(
	HDLE  	handle,
	UINT64  CRLid
);

void xapi_SetScrollBarPosition(
	HDLE  	handle,
	UINT64  CRLid,
	UINT64  position
);

UINT64 xapi_PutTextInputBox(
	HDLE    handle,
	UINT64  x,
	UINT64  y,
	UINT64  width,
	WSTR    text
);

void xapi_GetTextInputBox(
	UINT64 id,
	WSTR   text
);

void xapi_DeleteTextInputBox(
	UINT64 id
);

void xapi_RegisterRightButtonMenu(
	HDLE  			handle,
	RightMenuItem  *items,
	UINT64 			count
);

void xapi_DeleteRightButtonMenu(HDLE handle);

#ifdef __cplusplus
}

inline INT32 xapi_DrawSvg(HDLE handle, UINT32 x, UINT32 y, UINT32 width, WSTR svgText)
{
	return xapi_DrawSvg(handle, x, y, width, svgText, false);
}

inline INT32 xapi_DrawFA(HDLE handle, UINT32 x, UINT32 y, UINT32 width, char *name)
{
	return xapi_DrawFA(handle, x, y, width, name, false);
}
#endif
