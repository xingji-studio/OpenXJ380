/*
 *
 *      XINGJI XJ380 Application Compile Tools (XACT) 
 *      XACT C\C++ Header
 *      XJ380API XAPI Edition TUI Part
 *      Release Version 1.2.0 - 2026/1/16
 *      Copyright(C) XINGJI Interactive Software 2017 - 2026 All rights reserved.
 * 
 *      A XINGJI Interactive Software Production
 * 
 */

#pragma once

#include "stdint.h"
#include "./libsys.h"
#include "./liballoc/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

// define
#define PTE_PRESENT    (0x1 << 0)
#define PTE_WRITEABLE  (0x1 << 1)
#define PTE_USER       (0x1 << 2)
#define PTE_FLAG_U     (0x1 << 3)
#define PTE_HUGE       (0x1 << 7)
#define PTE_NO_EXECUTE (((uint64_t)0x1) << 63)

typedef enum {
    XUT_Root,
    XUT_System,
    XUT_Admin,
    XUT_Visitor,
    XUT_Custom
} UserType;

typedef struct {
	UINT64	length;
	void*	buffer;
} XFILE;

typedef struct {
    char            name[64];   // 用户名
    UserType        user_type;  // 用户等级
} UserInfo;

typedef struct {
    char     name[64];
    UserType user_type;
} LoginUserInfo;

typedef struct {
	char	 filename[256];
	UINT64	 length;
	UINT64	 filetype;
}  DirNode;

typedef enum {
    XAPI_FILE_DIALOG_OPEN = 0,
    XAPI_FILE_DIALOG_SAVE = 1
} XApiFileDialogMode;

typedef struct
{
    int tm_sec;   // 秒数 [0，59]
    int tm_min;   // 分钟数 [0，59]
    int tm_hour;  // 小时数 [0，59]
    int tm_mday;  // 1 个月的天数 [1，31]
    int tm_mon;   // 1 年中月份 [1，12]
    int tm_year;  // 年数
    int tm_wday;  // 1 星期中的某天 [1，7] (星期日 = 1)
    int tm_yday;  // 1 年中的某天 [0，365]
    int tm_isdst; // 夏令时标志
} TimeType;

// 3-1
void xapi_Output(WSTR str);
void xapi_Input(WSTR str);
char xapi_Getch(void);
void xapi_EndLine(void);
void xapi_PrintLine(WSTR str);
void xapi_Printf(WSTR fmt, ...);
void xapi_OutputSerial(WSTR str);

// 3-2
XFILE *xapi_OpenFile(
	WSTR	path
);

void xapi_CloseFile(XFILE* fsptr); 

void  xapi_SearchFile(
	WSTR	 path,
	INT32	*count,
	DirNode	**dir
);
void xapi_SearchFile_freem(DirNode *dir, INT32 count);

UINT64 xapi_Mkdir(WSTR path);
UINT64 xapi_CreateFile(WSTR filename);
UINT64 xapi_DeleteFile(WSTR path);
UINT64 xapi_RenameFile(WSTR old_path, WSTR new_path);
UINT64 xapi_ReadFile(WSTR filename, char *buffer, UINT64 size, UINT64 offset);
UINT64 xapi_WriteFile(WSTR filename, char *buffer, UINT64 size, UINT64 offset);
UINT64 xapi_Rmdir(WSTR path);
UINT64 xapi_FileDialog(UINT64 mode, WSTR title, WSTR initial_path, char *out_path, UINT64 out_size);

// 3-3
UINT64 	xcr_char2int(WSTR str);
WSTR 	xcr_int2char(UINT64 dec); 
WSTR 	xcr_hex2char(UINT64  hex);
UINT32 	toRGB(UINT8 r, UINT8 g, UINT8 b);
UINT32 	toRGBA(UINT32 color);

// 3-4
UINT64  xapi_Fork(void);
UINT64  xapi_Execve(WSTR filename, WSTR argv[], WSTR envp[]);
void	xapi_Exit(UINT64 value);
UINT64  xapi_GetTaskList(XapiTaskInfo *buffer, UINT64 max_count);
UINT64  xapi_KillProcess(UINT64 pid);

// 3-5
void	xapi_GetSystemVersion(WSTR version);
UINT64	xapi_GetTime(void);
void	xapi_GetCurrentUser(UserInfo *user_info);
UINT64  xapi_UserOobeRequired(void);
UINT64  xapi_UserList(LoginUserInfo *buffer, UINT64 max_count);
UINT64  xapi_UserLogin(WSTR username, WSTR password);
UINT64  xapi_UserCreateFirst(WSTR username, WSTR password);
void	xapi_GetTimeX(TimeType *tm);
void	xapi_GetCpuModel(WSTR version);
UINT64	xapi_GetMemorySize(void);
UINT64	xapi_GetTimeNano();

// 3-6
void	xapi_Broken(WSTR broken_info);
void	xapi_SendAppMessage(WSTR title, WSTR text);
UINT64  xapi_SetNotifyPrcor(XApiNotifyPrcor func);
UINT64  xapi_SendNotification(const XApiNotificationRequest *req);
void 	xapi_Sleep(UINT64 ms);
UINT64  xapi_PowerAction(UINT64 action);
void 	xapi_FlushTime(void);

// 3-7
void   *xapi_AllocateMemory(UINT64 size);
void    xapi_FreeMemory(void *ptr);
void   *xapi_MapMemory(void *ptr, uint64_t size, uint32_t flags);

#ifdef __cplusplus
}
#endif
