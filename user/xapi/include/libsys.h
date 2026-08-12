#pragma once

#include "./stdint.h"
#include "./krlibc.h"
#include "./liballoc/alloc.h"

// XJ380API POSIX Edition
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_FSTAT 5
#define SYS_LSTAT 6
#define SYS_POLL 7
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_MUNMAP 11
#define SYS_BRK 12

#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15

#define SYS_IOCTL 16

#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_ACCESS 21

#define SYS_PIPE 22
#define SYS_SELECT 23
#define SYS_SCHED_YIELD 24

#define SYS_MREMAP 25
#define SYS_MSYNC 26
#define SYS_MINCORE 27

#define SYS_DUP 32
#define SYS_DUP2 33
#define SYS_DUP3 292
#define SYS_PAUSE 34
#define SYS_NANOSLEEP 35

#define SYS_ALARM 37

#define SYS_GETPID 39
#define SYS_SENDFILE 40

#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_SOCKETPAIR 53

#define SYS_SHUTDOWN 48

#define SYS_BIND 49
#define SYS_LISTEN 50

#define SYS_CLONE 56
#define SYS_FORK 57
#define SYS_VFORK 58
#define SYS_EXECVE 59
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_KILL 62
#define SYS_UNAME 63
#define SYS_FCNTL 72
#define SYS_FTRUNCATE 77

#define SYS_GETDENTS 78
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_FCHDIR 81
#define SYS_RENAME 82

#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_CREAT 85
#define SYS_LINK 86
#define SYS_UNLINK 87
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_FCHMOD 91

#define SYS_SYSINFO 99
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_SETUID 105
#define SYS_SETGID 106
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_SETSID 112
#define SYS_GETGROUPS 115
#define SYS_GETSID 124
#define SYS_GETPGID 121
#define SYS_MKNOD 133
#define SYS_STATFS 137
#define SYS_PIVOT_ROOT 155

#define SYS_ARCH_PRCTL 158
#define SYS_MOUNT 165
#define SYS_UMOUNT2 166

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_SET_GS 0x1004
#define ARCH_GET_GS 0x1005

#define SYS_GETTID 186
#define SYS_TIME 201
#define SYS_GETDENTS64 217

#define SYS_SET_TID_ADDRESS 218
#define SYS_FADVISE64 221

#define SYS_CLOCK_GETTIME 228
#define SYS_CLOCK_GETRES 229
#define SYS_CLOCK_NANOSLEEP 230

#define SYS_EXIT_GROUP 231
#define SYS_TGKILL 234

#define SYS_OPENAT 257
#define SYS_MKDIRAT 258
#define SYS_NEWFSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_LINKAT 265
#define SYS_SYMLINKAT 266
#define SYS_READLINKAT 267
#define SYS_FACCESSAT 269
#define SYS_PSELECT6 270
#define SYS_SET_ROBUST_LIST 273
#define SYS_PIPE2 293
#define SYS_FSYNC 74
#define SYS_FDATASYNC 75
#define SYS_FSTATFS 138
#define SYS_COPY_FILE_RANGE 326
#define SYS_STATX 332

#define SYS_RSEQ 334
#define SYS_FSOPEN 430
#define SYS_FACCESSAT2 439



// XJ380API XAPI Edition
#define XAPI_OFFEST 7380

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

#define XAPI_SEARCH_FILE       7416
#define XAPI_SEARCH_FILE_FREEM 7471

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

#define XAPI_USER_OOBE_REQUIRED 7452
#define XAPI_USER_LIST          7453
#define XAPI_USER_LOGIN         7454
#define XAPI_USER_CREATE_FIRST  7455

#define XAPI_FLUSH_TIME     7445
#define XAPI_POWER_ACTION   7464

// P3.6.2
#define XAPI_NOTIFY_SEND    7465
#define XAPI_NOTIFY_SET_PROCOR 7466

#define XPOWER_REBOOT   1
#define XPOWER_SHUTDOWN 2

typedef enum {
    XNOTIFY_ICON_NONE = 0,
    XNOTIFY_ICON_INFO = 1,
    XNOTIFY_ICON_SUCCESS = 2,
    XNOTIFY_ICON_WARNING = 3,
    XNOTIFY_ICON_ERROR = 4,
    XNOTIFY_ICON_APP = 5
} XNotifyBuiltinIcon;

typedef struct {
    UINT64 key;
    char  *title;
    char  *text;
    UINT64 builtin_icon;
    char  *icon_path;
    char  *action0_text;
    UINT64 action0_id;
    char  *action1_text;
    UINT64 action1_id;
} XApiNotificationRequest;

typedef void (*XApiNotifyPrcor)(UINT64 notification_id, UINT64 action_id);

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

#define XAPI_TASK_NAME_LEN 32

typedef struct
{
    UINT64 pid;
    UINT64 ppid;
    UINT64 tid;
    UINT64 cpu_id;
    UINT64 task_level;
    UINT64 thread_count;
    UINT64 window_count;
    UINT64 memory_bytes;
    UINT32 process_status;
    UINT32 thread_status;
    char   process_name[XAPI_TASK_NAME_LEN];
    char   thread_name[XAPI_TASK_NAME_LEN];
} XapiTaskInfo;

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

#define XWIN_NORMAL             0
#define XWIN_FRAME_OFF          1
#define XWIN_FULL_SCR           2
#define XWIN_DESKTOP            3
#define XWIN_DOCK               4
#define XWIN_LOGIN              5
#define XWIN_TYPE_MASK          0x0f
#define XWIN_SUPPORT_RESIZEABLE 0x80

#ifdef __cplusplus
extern "C" {
#endif
uint64_t enter_syscall(uint64_t syscall_number, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
#ifdef __cplusplus
}
#endif
