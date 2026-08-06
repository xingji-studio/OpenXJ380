#pragma once

#include <proto.hpp>

typedef void (*MsgPrcor)(uint64_t type, uint64_t hdata, uint64_t ldata);

typedef  struct {
	char	    filename[256];
	uint64_t	length;
	uint64_t	filetype;
}  XAPIT_DirNode;

typedef struct {
	uint64_t    length;
	void*	    buffer;
    // 以下成员隐藏
    char path[256];
} XFILE;


// XJ380API XAPI Edition
#define XAPI_OFFEST 380

// Private extension syscall numbers. They are internal ABI, not a security boundary.
#define SXAH_SYSCALL_RETURN         128956723895689201      // syscall返回
#define SXAH_CREATE_KERNEL_TERMINAL 128956723895689202      // 创建内核态进程
#define SXAH_CHECK_USER_PASSWORD    128956723895689203      // 检查用户密码
#define SXAH_MARK_IS_TERMINAL       128956723895689204      // 标记为命令行
#define SXAH_WRITE_INPUT_BUFFER     128956723895689205      // 写入XTTTP输入缓冲区
#define SXAH_READ_OUTPUT_BUFFER     128956723895689206      // 读取XTTTP输出缓冲区
#define SXAH_CHECK_INPUT_BUFFER     128956723895689207      // 检查是否需要输入
#define SXAH_UNLOCK_OUTPUT_LOCK     128956723895689208      // 完成输出，关闭输出锁
#define SXAH_MESSAGE_ASK            128956723895689209      // message查询
#define SXAH_INSTALLER_ENUM_DISKS       128956723895689220
#define SXAH_INSTALLER_START            128956723895689221
#define SXAH_INSTALLER_PROGRESS         128956723895689222
#define SXAH_INSTALLER_PRECHECK         128956723895689223
#define SXAH_INSTALLER_START_EX         128956723895689224
#define SXAH_INSTALLER_RESCUE           128956723895689225
#define SXAH_INSTALLER_LOG              128956723895689226
#define SXAH_INSTALLER_START_OPTIONS    128956723895689227
#define SXAH_INSTALLER_PRECHECK_OPTIONS 128956723895689228

#define SXAH_CHECK_TERMINAL_INIT_STATUS  3801    // 查询终端初始化状态

typedef struct
{
    MsgPrcor WinMpf;
    uint64_t msg_type;
    uint64_t lData;
    uint64_t hData;
} MessageInfoFormat;


// define
typedef struct {
    char            name[64];   // 用户名
    UserType        user_type;  // 用户等级
} xapi_type_UserInfo;

typedef struct {
    char     name[64];
    UserType user_type;
} xapi_type_LoginUserInfo;

// P3.1
#define XAPI_OUTPUT    7381
#define XAPI_INPUT     7382
#define XAPI_INPUT_NO_ECHO 0x1

#define XAPI_GETLINE   7418

#define XAPI_GETCH          7383
#define XAPI_ENDLINE        7384
#define XAPI_PRINTLINE      7385
#define XAPI_PRINTF         7386
#define XAPI_OUTPUT_SERIAL  7427

// P3.2
#define XAPI_OPEN_FILE      7387
#define XAPI_CLOSE_FILE     7388

#define XAPI_SEARCH_FILE    7416

#define XAPI_MAKEDIR        7425
#define XAPI_CREATE_FILE    7420
#define XAPI_DELETE_FILE    7421
#define XAPI_RENAME_FILE    7422
#define XAPI_READ_FILE      7423
#define XAPI_WRITE_FILE     7424

#define XAPI_REMOVEDIR      7444
#define XAPI_FILE_DIALOG    7450

// P3.4
#define XAPI_FORK   7389
#define XAPI_EXECVE 7390
#define XAPI_GET_TASK_LIST  7448
#define XAPI_KILL_PROCESS   7449

// P3.5
#define XAPI_GET_VERSION        7391

#define XAPI_GET_TIME           7412
#define XAPI_GET_CURRENT_USER   7413

#define XAPI_GET_TIME_X         7433
#define XAPI_GET_CPU_MODEL      7434
#define XAPI_GET_MEMORY_SIZE    7435

#define XAPI_GET_TIME_NANO      7459

// P3.6
#define XAPI_BROKEN         7428
#define XAPI_SEND_APP_MSG   7429
#define XAPI_SLEEP          7430

#define XAPI_RUN            7439
#define XAPI_RUN_ARGS       7451
#define XAPI_USER_OOBE_REQUIRED 7452
#define XAPI_USER_LIST          7453
#define XAPI_USER_LOGIN         7454
#define XAPI_USER_CREATE_FIRST  7455

#define XAPI_FLUSH_TIME     7445
#define XAPI_POWER_ACTION   7464

// P3.6.2
#define XAPI_NOTIFY_SEND        7465
#define XAPI_NOTIFY_SET_PROCOR  7466

#define XPOWER_REBOOT   1
#define XPOWER_SHUTDOWN 2

typedef enum
{
    XNOTIFY_ICON_NONE = 0,
    XNOTIFY_ICON_INFO = 1,
    XNOTIFY_ICON_SUCCESS = 2,
    XNOTIFY_ICON_WARNING = 3,
    XNOTIFY_ICON_ERROR = 4,
    XNOTIFY_ICON_APP = 5,
} XNotifyBuiltinIcon;

typedef struct
{
    uint64_t key;
    char    *title;
    char    *text;
    uint64_t builtin_icon;
    char    *icon_path;
    char    *action0_text;
    uint64_t action0_id;
    char    *action1_text;
    uint64_t action1_id;
} XApiNotificationRequest;

typedef void (*XApiNotifyPrcor)(uint64_t notification_id, uint64_t action_id);

// P3.7
#define XAPI_MALLOC         7441
#define XAPI_FREE           7442
#define XAPI_MAP_MEMORY     7443

// P4.1
#define XAPI_CREATE_WINDOW    7392
#define XAPI_SET_WINDOW_TITLE 7393
#define XAPI_CLOSE_WINDOW     7394
#define XAPI_SET_ICON         7395

#define XAPI_GET_WIN_SZIE     7426

// P4.2
#define XAPI_DRAW_POINT       7396
#define XAPI_DRAW_LINE        7397
#define XAPI_DRAW_RECT        7398
#define XAPI_DRAW_RECT_FILL   7399
#define XAPI_DRAW_CIRCLE      7400
#define XAPI_DRAW_CIRCLE_FILL 7401
#define XAPI_DRAW_TEXT        7402

#define XAPI_DRAW_TEXT_L      7414
#define XAPI_DRAW_TEXT_SW     7415
#define XAPI_CALC_TEXT_WIDTH  7431

// P4.3
#define XAPI_DRAW_BMP       7403
#define XAPI_DRAW_PNG       7404

#define XAPI_DRAW_PICTURE   7419

#define XAPI_GET_PIC_SIZE   7440
#define XAPI_DRAW_SVG       7446
#define XAPI_DRAW_FA        7447

#define XAPI_LOAD_PICTURE   7463

// P4.4
#define XAPI_SET_MSH_PROCOR 7405

#define MSG_CHAR    0
#define MSG_MOVE    1
#define MSG_LBUTTON 2
#define MSG_RBUTTON 3
#define MSG_MBUTTON 4
#define MSG_ROLLER  5
#define MSG_CRL     6
#define MSG_SPCHAR  7
#define MSG_RESIZE  8
#define MSG_KEYUP   9
#define MSG_KEYDOWN 10
#define MSG_LBUTTONDOWN 11
#define MSG_LBUTTONUP   12

// P4.5
#define XAPI_READBUFFER     7406
#define XAPI_WRITEBUFFER    7407

#define XAPI_READBUFFERA    7417

#define XAPI_WRITEBUFFERA   7408
#define XAPI_REFRESH_WINDOW 7409

#define XAPI_REFRESH_PART_WINDOW 7438

// P4.6
#define XAPI_BUTTON         7410
#define XAPI_BUTTON_EMP     7411
#define XAPI_DELETE_BUTTON  7432

#define XAPI_REG_RB_MENU    7436
#define XAPI_URG_RB_MENU    7437

#define XAPI_PUT_TEXT_INBOX 7456
#define XAPI_GET_TEXT_INBOX 7457
#define XAPI_DEL_TEXT_INBOX 7458

#define XAPI_PUT_SWITCH     7460
#define XAPI_SET_SWITCH     7461
#define XAPI_DEL_SWITCH     7462

#define XAPI_PUT_VERTICAL_SCROLL_BAR   7467
#define XAPI_PUT_HORIZONTAL_SCROLL_BAR 7468
#define XAPI_DELETE_SCROLL_BAR         7469
#define XAPI_SET_SCROLL_BAR_POSITION   7470

// 4-1
#define XWIN_NORMAL				0
#define XWIN_FRAME_OFF			1
#define XWIN_FULL_SCR			2
#define XWIN_DESKTOP            3
#define XWIN_DOCK               4
#define XWIN_LOGIN              5
#define XWIN_TYPE_MASK          0x0f
#define XWIN_SUPPORT_RESIZEABLE 0x80

// 当前最大号：7470

#define XAPI_TASK_NAME_LEN 32

typedef struct
{
    uint64_t pid;
    uint64_t ppid;
    uint64_t tid;
    uint64_t cpu_id;
    uint64_t task_level;
    uint64_t thread_count;
    uint64_t window_count;
    uint64_t memory_bytes;
    uint32_t process_status;
    uint32_t thread_status;
    char     process_name[XAPI_TASK_NAME_LEN];
    char     thread_name[XAPI_TASK_NAME_LEN];
} XapiTaskInfo;

// Proto (XAPI Edition)
void do_xapi_Output(char *str);
int  do_xapi_Input(char *str, size_t capacity, uint64_t flags);
char do_xapi_Getch();
void do_xapi_Endline();
void do_xapi_Printline(char *str);
void do_xapi_OutputSerial(char *str);
void do_xapi_SearchFile(uint64_t path, uint64_t count, uint64_t dir);
void do_xapi_GetSystemVersion(uint64_t str);
uint64_t do_xapi_GetTime();
void do_xapi_GetCurrentUser(uint64_t dst);
uint64_t do_xapi_ReadFile(struct X64_REGS *regs);
uint64_t do_xapi_WriteFile(struct X64_REGS *regs);
void do_xapi_Printf(char *str);
void do_xapi_Sleep(uint64_t ms);
uint64_t do_xapi_OpenFile(uint64_t path);
void do_xapi_CloseFile(uint64_t ptr);
void do_xapi_Broken(char *info);
void do_xapi_GetTimeX(uint64_t tt);
uint64_t do_xapi_GetMemorySize();
void do_xapi_Run(char *path);
uint64_t do_xapi_RunArgs(char *path, char **argv);
uint64_t do_xapi_MapMemory(uint64_t ptr, uint64_t size, uint32_t flags);
void do_xapi_FlushTime();
uint64_t do_xapi_GetTaskList(uint64_t buffer, uint64_t max_count);
uint64_t do_xapi_KillProcess(uint64_t pid);
uint64_t do_xapi_UserOobeRequired();
uint64_t do_xapi_UserList(uint64_t buffer, uint64_t max_count);
uint64_t do_xapi_UserLogin(uint64_t username, uint64_t password);
uint64_t do_xapi_UserCreateFirst(uint64_t username, uint64_t password);
uint64_t do_xapi_PowerAction(uint64_t action);
uint64_t do_xapi_SendAppMessage(char *title, char *text);
uint64_t do_xapi_SendNotification(const XApiNotificationRequest *req);
uint64_t do_xapi_SetNotifyPrcor(uint64_t func);
uint64_t do_xapi_InstallerEnumDisks(uint64_t list);
uint64_t do_xapi_InstallerStart(uint64_t disk_id);
uint64_t do_xapi_InstallerStartEx(uint64_t disk_id, uint64_t mode);
uint64_t do_xapi_InstallerStartOptions(uint64_t options);
uint64_t do_xapi_InstallerPrecheck(uint64_t disk_id, uint64_t mode, uint64_t out);
uint64_t do_xapi_InstallerPrecheckOptions(uint64_t options, uint64_t out);
uint64_t do_xapi_InstallerProgress(uint64_t progress);
uint64_t do_xapi_InstallerRescue(uint64_t action, uint64_t disk_id, uint64_t out);
uint64_t do_xapi_InstallerLog(uint64_t out);



void init_syscall();

// Proto (XAPI Edition)
void do_message(uint64_t msg_type, uint64_t hData, uint64_t lData, MsgPrcor WinMpf, tcb_t ftask);
void init_message();
bool init_notify_message(pcb_t process, uint64_t func);
void message_thread(uint64_t arg);
uint64_t message_ask(uint64_t msg_type_p, uint64_t hdatap, uint64_t ldatap, uint64_t funcp, uint64_t taskp);
