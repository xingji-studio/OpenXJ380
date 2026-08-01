#include "./xapi/include/x3api.h"

static volatile UINT64 g_last_notification_id = 0;
static volatile UINT64 g_last_action_id       = 0;
static volatile UINT64 g_callback_count       = 0;

static void log_line(const char *message)
{
    xapi_OutputSerial((char *)message);
}

static void log_u64(const char *prefix, UINT64 value)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s%llu\n", prefix, value);
    log_line(buf);
}

static void notify_callback(UINT64 notification_id, UINT64 action_id)
{
    g_last_notification_id = notification_id;
    g_last_action_id       = action_id;
    g_callback_count++;

    char buf[192];
    snprintf(buf, sizeof(buf), "msgtest：回调 notification=%llu action=%llu count=%llu\n",
             notification_id, action_id, g_callback_count);
    log_line(buf);
}

static UINT64 send_notification(const char *name,
                                UINT64 key,
                                const char *title,
                                const char *text,
                                UINT64 icon,
                                const char *icon_path,
                                const char *action0_text,
                                UINT64 action0_id,
                                const char *action1_text,
                                UINT64 action1_id,
                                UINT64 delay_ms)
{
    XApiNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.key          = key;
    req.title        = (char *)title;
    req.text         = (char *)text;
    req.builtin_icon = icon;
    req.icon_path    = (char *)icon_path;
    req.action0_text = (char *)action0_text;
    req.action0_id   = action0_id;
    req.action1_text = (char *)action1_text;
    req.action1_id   = action1_id;

    char logbuf[192];
    snprintf(logbuf, sizeof(logbuf), "msgtest：发送 %s\n", name);
    log_line(logbuf);

    UINT64 ret = xapi_SendNotification(&req);
    snprintf(logbuf, sizeof(logbuf), "msgtest：返回 %s = %lld\n", name, (long long)ret);
    log_line(logbuf);

    if (delay_ms > 0) xapi_Sleep(delay_ms);
    return ret;
}

static int msgtest_main_impl(int argc, char *argv[], char *envp[])
{
    (void)envp;

    if (argc > 1 && strcmp(argv[1], "--expect-nondesktop") == 0)
    {
        UINT64 ret = send_notification("nondesktop",
                                       0,
                                       "非桌面态通知",
                                       "非桌面态下预期返回 0。",
                                       XNOTIFY_ICON_WARNING,
                                       NULL,
                                       NULL,
                                       0,
                                       NULL,
                                       0,
                                       0);
        log_u64("msgtest：非桌面态返回=", ret);
        return ret == 0 ? 0 : 1;
    }

    log_line("msgtest：开始 XAPI 3.6.2 toast 通知测试\n");
    UINT64 set_ret = xapi_SetNotifyPrcor(notify_callback);
    log_u64("msgtest：设置通知处理器返回=", set_ret);

    xapi_SendAppMessage((char *)"兼容 SendAppMessage",
                        (char *)"旧 API 应立即返回，并显示 info toast。");
    log_line("msgtest：兼容调用已返回\n");
    xapi_Sleep(500);

    send_notification("overflow-1", 0, "溢出 1", "当前最老的可见 toast。", XNOTIFY_ICON_INFO, NULL, NULL, 0, NULL, 0, 250);
    send_notification("overflow-2", 0, "溢出 2", "第二条可见 toast。", XNOTIFY_ICON_SUCCESS, NULL, NULL, 0, NULL, 0, 250);
    send_notification("overflow-3", 0, "溢出 3", "第三条可见 toast。", XNOTIFY_ICON_WARNING, NULL, NULL, 0, NULL, 0, 250);
    send_notification("overflow-4", 0, "溢出 4", "这条应淘汰溢出 1。", XNOTIFY_ICON_ERROR, NULL, NULL, 0, NULL, 0, 600);

    UINT64 key_first = send_notification("key-first",
                                         362,
                                         "带 key 的 Toast",
                                         "第一版。ID 应被复用。",
                                         XNOTIFY_ICON_APP,
                                         NULL,
                                         NULL,
                                         0,
                                         NULL,
                                         0,
                                         700);
    UINT64 key_second = send_notification("key-update",
                                          362,
                                          "带 key 的 Toast 已更新",
                                          "原地更新。计时重置且 ID 稳定。",
                                          XNOTIFY_ICON_APP,
                                          NULL,
                                          NULL,
                                          0,
                                          NULL,
                                          0,
                                          700);
    log_u64("msgtest：key 首次 ID=", key_first);
    log_u64("msgtest：key 更新 ID=", key_second);

    send_notification("path-icon",
                      0,
                      "路径图标",
                      "可用时使用真实资源路径。",
                      XNOTIFY_ICON_INFO,
                      "/system/icon/computer.png",
                      NULL,
                      0,
                      NULL,
                      0,
                      600);
    send_notification("bad-path-icon",
                      0,
                      "图标回退",
                      "错误图标路径应回退到 warning 图标。",
                      XNOTIFY_ICON_WARNING,
                      "/system/icon/does-not-exist.png",
                      NULL,
                      0,
                      NULL,
                      0,
                      600);

    send_notification("actions",
                      0,
                      "动作 Toast",
                      "点击任一动作按钮；串口日志应显示不同 action_id。",
                      XNOTIFY_ICON_INFO,
                      NULL,
                      "动作 A",
                      1001,
                      "动作 B",
                      1002,
                      0);

    send_notification("close-only",
                      0,
                      "关闭按钮测试",
                      "点击 X：应关闭且不回调。",
                      XNOTIFY_ICON_SUCCESS,
                      NULL,
                      NULL,
                      0,
                      NULL,
                      0,
                      0);

    log_line("msgtest：手动检查：点击动作按钮和关闭按钮，然后观察串口输出\n");
    xapi_Sleep(9000);
    log_u64("msgtest：回调次数=", g_callback_count);
    log_u64("msgtest：最后动作=", g_last_action_id);
    log_line("msgtest：完成\n");
    return 0;
}

extern "C" int msgtest_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int msgtest_main_cpp(int argc, char *argv[], char *envp[])
{
    return msgtest_main_impl(argc, argv, envp);
}
