#include <x3api.h>
#include <xj380_i18n.h>
#include <string.h>

static const UINT32 RUN_WIDTH = 520;
static const UINT32 RUN_HEIGHT = 210;
static const UINT64 RUN_BTN_OK = 1001;
static const UINT64 RUN_BTN_CANCEL = 1002;
static const int RUN_INPUT_MAX = 256;
static const int RUN_ARG_MAX = 16;

static HDLE g_handle = 0;
static UINT64 g_input_id = 0;
static bool g_running = true;
static char g_status[96] = "";
static const char *g_status_zh = "请输入 ELF 文件路径";
static const char *g_status_en = "Enter an ELF path";
static int g_language = XJ380_LANGUAGE_ZH_CN;

static char *run_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static void draw_status()
{
    xapi_DrawRect(g_handle, 16, 150, 310, 177, 0xffffffff, true);
    xapi_DrawSWText(g_handle, 18, 154, g_status, 0x4b5563ff);
    xapi_RefreshPartWindow(g_handle, 16, 150, 310, 177);
}

static void refresh_status_translation()
{
    strncpy(g_status, run_tr(g_status_zh, g_status_en), sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    if (g_handle != 0) draw_status();
}

static void set_status_text(const char *zh_cn, const char *en_us)
{
    g_status_zh = zh_cn == NULL ? "" : zh_cn;
    g_status_en = en_us == NULL ? "" : en_us;
    refresh_status_translation();
}

static void close_run_dialog()
{
    if (g_input_id != 0)
    {
        xapi_DeleteTextInputBox(g_input_id);
        g_input_id = 0;
    }
    xapi_CloseWindow(g_handle);
    g_running = false;
}

static void recreate_input_box(const char *text)
{
    if (g_input_id != 0)
    {
        xapi_DeleteTextInputBox(g_input_id);
        g_input_id = 0;
    }
    g_input_id = xapi_PutTextInputBox(g_handle, 70, 112, 420,
                                      (char *)(text == NULL ? "/apps/system/" : text));
}

static bool split_command(char *command, char **argv)
{
    if (command == NULL || argv == NULL) return false;

    while (*command == ' ' || *command == '\t') command++;
    if (*command == '\0') return false;

    int argc = 0;
    char *cursor = command;
    while (*cursor != '\0' && argc < RUN_ARG_MAX - 1)
    {
        while (*cursor == ' ' || *cursor == '\t')
        {
            *cursor = '\0';
            cursor++;
        }
        if (*cursor == '\0') break;

        argv[argc++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') cursor++;
    }
    argv[argc] = NULL;
    return argc > 0;
}

static void run_command()
{
    char command[RUN_INPUT_MAX];
    memset(command, 0, sizeof(command));
    xapi_GetTextInputBox(g_input_id, command);

    char *argv[RUN_ARG_MAX];
    memset(argv, 0, sizeof(argv));
    if (!split_command(command, argv))
    {
        set_status_text("请输入要运行的 ELF 路径", "Enter the ELF path to run");
        return;
    }

    INT64 pid = (INT64)xapi_RunArgs(argv[0], argv);
    if (pid <= 0)
    {
        set_status_text("启动失败，请检查路径是否正确", "Launch failed. Check the path.");
        return;
    }

    close_run_dialog();
}

static void draw_run_dialog()
{
    xapi_DrawRect(g_handle, 0, 0, RUN_WIDTH - 1, RUN_HEIGHT - 1, 0xf4f8ffff, true);
    xapi_DrawRect(g_handle, 0, 0, RUN_WIDTH - 1, 58, 0x2474c6ff, true);
    xapi_DrawRect(g_handle, 0, 58, RUN_WIDTH - 1, RUN_HEIGHT - 1, 0xffffffff, true);
    xapi_DrawSWText(g_handle, 18, 18, run_tr("运行", "Run"), 0xffffffff);
    xapi_DrawSWText(g_handle, 18, 78, run_tr("输入 ELF 文件路径，按 Enter 或“确定”运行。",
                                             "Enter an ELF path, then press Enter or OK."),
                    0x1d3557ff);
    xapi_DrawSWText(g_handle, 18, 116, run_tr("路径:", "Path:"), 0x111111ff);
    draw_status();

    xapi_DeleteButton(g_handle, RUN_BTN_OK);
    xapi_DeleteButton(g_handle, RUN_BTN_CANCEL);
    xapi_Button(g_handle, RUN_BTN_OK, 332, 160, run_tr("确定", "OK"));
    xapi_ButtonEmp(g_handle, RUN_BTN_CANCEL, 418, 160, run_tr("取消", "Cancel"));
    xapi_RefreshWindow(g_handle);
}

static void run_message(UINT64 type, UINT64 hData, UINT64 lData)
{
    switch (type)
    {
    case MSG_CRL:
        if (hData == RUN_BTN_OK) run_command();
        else if (hData == RUN_BTN_CANCEL) close_run_dialog();
        break;
    case MSG_KEYDOWN:
        if (lData == '\n') run_command();
        else if (lData == XKEY_ESC) close_run_dialog();
        break;
    default:
        break;
    }
}

static int elfrun_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;
    g_language = xj380_read_language();
    set_status_text("请输入 ELF 文件路径", "Enter an ELF path");

    XWINDOW window;
    window.title = run_tr("运行", "Run");
    window.width = RUN_WIDTH;
    window.height = RUN_HEIGHT;
    window.sets = XWIN_NORMAL;
    xapi_CreateWindow(&g_handle, &window);
    xapi_SetIcon(g_handle, (char *)"/system/icon/unknowexe.png");
    SetMsgPrcor(g_handle, run_message);

    draw_run_dialog();
    recreate_input_box("/apps/system/");

    while (g_running)
    {
        xapi_Sleep(16);
    }
    return 0;
}

extern "C" int elfrun_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int elfrun_main_cpp(int argc, char *argv[], char *envp[])
{
    return elfrun_main_impl(argc, argv, envp);
}
