#include <x3api.h>
#include <krlibc.h>
#include <xj380_i18n.h>

#define TASKMGR_WIDTH  860
#define TASKMGR_HEIGHT 540
#define TASKMGR_MAX_TASKS 192
#define TASKMGR_ROW_H 22
#define TASKMGR_TABLE_Y 96

#define COLOR_BG        0xf4f6f8ff
#define COLOR_HEADER    0x203040ff
#define COLOR_PANEL     0xffffffff
#define COLOR_LINE      0xd7dde4ff
#define COLOR_TEXT      0x14202bff
#define COLOR_MUTED     0x667381ff
#define COLOR_SELECTED  0xdbeafeff
#define COLOR_DANGER    0xb42318ff
#define COLOR_PRIMARY   0x1f6febff
#define COLOR_DISABLED  0xa9b2bdff

static HDLE          g_window;
static XapiTaskInfo  g_tasks[TASKMGR_MAX_TASKS];
static UINT64        g_task_count = 0;
static int           g_scroll_top = 0;
static int           g_selected_index = -1;
static UINT64        g_selected_pid = 0;
static UINT64        g_selected_tid = 0;
static UINT64        g_width = TASKMGR_WIDTH;
static UINT64        g_height = TASKMGR_HEIGHT;
static char          g_status[128] = "就绪。";
static bool          g_need_redraw = true;
static int           g_language = XJ380_LANGUAGE_ZH_CN;

static char *taskmgr_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static void set_status_text(const char *zh_cn, const char *en_us)
{
    memset(g_status, 0, sizeof(g_status));
    strncpy(g_status, taskmgr_tr(zh_cn, en_us), sizeof(g_status) - 1);
}

static const char *status_name(UINT32 status)
{
    switch (status)
    {
    case 0: return taskmgr_tr("创建", "Created");
    case 1: return taskmgr_tr("运行", "Running");
    case 2: return taskmgr_tr("等待", "Waiting");
    case 3: return taskmgr_tr("结束", "Ended");
    case 4: return taskmgr_tr("启动", "Starting");
    case 5: return taskmgr_tr("互斥", "Mutex");
    case 6: return taskmgr_tr("退出", "Exited");
    case 7: return taskmgr_tr("僵尸", "Zombie");
    default: return "?";
    }
}

static const char *level_name(UINT64 level)
{
    switch (level)
    {
    case 0: return taskmgr_tr("内核", "Kernel");
    case 1: return taskmgr_tr("空闲", "Idle");
    case 2: return taskmgr_tr("应用", "App");
    default: return "?";
    }
}

static void clamp_scroll()
{
    int visible_rows = ((int)g_height - TASKMGR_TABLE_Y - 56) / TASKMGR_ROW_H;
    if (visible_rows < 1) visible_rows = 1;
    int max_top = (int)g_task_count - visible_rows;
    if (max_top < 0) max_top = 0;
    if (g_scroll_top < 0) g_scroll_top = 0;
    if (g_scroll_top > max_top) g_scroll_top = max_top;
}

static void remember_selection()
{
    if (g_selected_index >= 0 && g_selected_index < (int)g_task_count)
    {
        g_selected_pid = g_tasks[g_selected_index].pid;
        g_selected_tid = g_tasks[g_selected_index].tid;
    }
}

static void restore_selection()
{
    g_selected_index = -1;
    for (int i = 0; i < (int)g_task_count; ++i)
    {
        if (g_tasks[i].pid == g_selected_pid && g_tasks[i].tid == g_selected_tid)
        {
            g_selected_index = i;
            break;
        }
    }
}

static void load_tasks()
{
    remember_selection();
    memset(g_tasks, 0, sizeof(g_tasks));
    g_task_count = xapi_GetTaskList(g_tasks, TASKMGR_MAX_TASKS);
    if (g_task_count > TASKMGR_MAX_TASKS) g_task_count = TASKMGR_MAX_TASKS;
    restore_selection();
    clamp_scroll();
}

static bool selected_process_can_end()
{
    if (g_selected_index < 0 || g_selected_index >= (int)g_task_count) return false;
    return g_tasks[g_selected_index].task_level == 2;
}

static void draw_button(int x1, int y1, int x2, int y2, const char *text, UINT32 color)
{
    xapi_DrawRect(g_window, x1, y1, x2, y2, color, true);
    xapi_DrawRect(g_window, x1, y1, x2, y2, 0x00000024, false);
    xapi_DrawSWText(g_window, x1 + 12, y1 + 8, (char *)text, 0xffffffff);
}

static void draw_task_row(int row, int task_index)
{
    int y = TASKMGR_TABLE_Y + row * TASKMGR_ROW_H;
    bool selected = task_index == g_selected_index;
    XapiTaskInfo *task = &g_tasks[task_index];

    xapi_DrawRect(g_window, 18, y, (UINT32)g_width - 18, y + TASKMGR_ROW_H - 1,
                  selected ? COLOR_SELECTED : COLOR_PANEL, true);

    char pid[16], tid[16], cpu[16], wins[16], mem[24], threads[16];
    uint64_to_string(task->pid, pid, sizeof(pid));
    uint64_to_string(task->tid, tid, sizeof(tid));
    uint64_to_string(task->cpu_id, cpu, sizeof(cpu));
    uint64_to_string(task->window_count, wins, sizeof(wins));
    uint64_to_string(task->thread_count, threads, sizeof(threads));

    UINT64 mem_kb = task->memory_bytes / 1024;
    if (mem_kb >= 1024)
    {
        UINT64 mem_mb = mem_kb / 1024;
        uint64_to_string(mem_mb, mem, sizeof(mem));
        strcat(mem, "M");
    }
    else
    {
        uint64_to_string(mem_kb, mem, sizeof(mem));
        strcat(mem, "K");
    }

    xapi_DrawSWText(g_window, 26, y + 5, pid, COLOR_TEXT);
    xapi_DrawSWText(g_window, 72, y + 5, tid, COLOR_TEXT);
    xapi_DrawSWText(g_window, 122, y + 5, task->process_name, COLOR_TEXT);
    xapi_DrawSWText(g_window, 266, y + 5, task->thread_name, COLOR_TEXT);
    xapi_DrawSWText(g_window, 410, y + 5, (char *)status_name(task->process_status), COLOR_MUTED);
    xapi_DrawSWText(g_window, 494, y + 5, (char *)status_name(task->thread_status), COLOR_MUTED);
    xapi_DrawSWText(g_window, 578, y + 5, (char *)level_name(task->task_level), COLOR_MUTED);
    xapi_DrawSWText(g_window, 642, y + 5, threads, COLOR_TEXT);
    xapi_DrawSWText(g_window, 702, y + 5, wins, COLOR_TEXT);
    xapi_DrawSWText(g_window, 754, y + 5, mem, COLOR_TEXT);
    xapi_DrawSWText(g_window, 816, y + 5, cpu, COLOR_MUTED);

    xapi_DrawLine(g_window, 18, y + TASKMGR_ROW_H - 1, (UINT32)g_width - 18, y + TASKMGR_ROW_H - 1, COLOR_LINE);
}

static void render()
{
    g_language = xj380_read_language();
    xapi_GetWindowSize(g_window, &g_width, &g_height);
    clamp_scroll();
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, COLOR_BG, true);
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, 56, COLOR_HEADER, true);
    xapi_DrawSWText(g_window, 20, 18, taskmgr_tr("任务管理器", "Task Manager"), 0xffffffff);

    char summary[96];
    char count_text[24];
    uint64_to_string(g_task_count, count_text, sizeof(count_text));
    strcpy(summary, taskmgr_tr("线程数: ", "Threads: "));
    strcat(summary, count_text);
    strcat(summary, taskmgr_tr("    单击一行后可结束应用进程。", "    Select an app row to end it."));
    xapi_DrawSWText(g_window, 160, 18, summary, 0xdce6f2ff);

    draw_button((int)g_width - 240, 16, (int)g_width - 156, 42, taskmgr_tr("刷新", "Refresh"), COLOR_PRIMARY);
    draw_button((int)g_width - 144, 16, (int)g_width - 24, 42, taskmgr_tr("结束进程", "End Process"),
                selected_process_can_end() ? COLOR_DANGER : COLOR_DISABLED);

    xapi_DrawRect(g_window, 18, 70, (UINT32)g_width - 18, (UINT32)g_height - 48, COLOR_PANEL, true);
    xapi_DrawRect(g_window, 18, 70, (UINT32)g_width - 18, (UINT32)g_height - 48, COLOR_LINE, false);

    xapi_DrawSWText(g_window, 26, 78, (char *)"PID", COLOR_MUTED);
    xapi_DrawSWText(g_window, 72, 78, (char *)"TID", COLOR_MUTED);
    xapi_DrawSWText(g_window, 122, 78, taskmgr_tr("进程", "Process"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 266, 78, taskmgr_tr("线程", "Thread"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 410, 78, taskmgr_tr("进程态", "P-State"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 494, 78, taskmgr_tr("线程态", "T-State"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 578, 78, taskmgr_tr("层级", "Level"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 642, 78, taskmgr_tr("线程", "Th"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 702, 78, taskmgr_tr("窗口", "Win"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 754, 78, taskmgr_tr("内存", "Memory"), COLOR_MUTED);
    xapi_DrawSWText(g_window, 816, 78, (char *)"CPU", COLOR_MUTED);
    xapi_DrawLine(g_window, 18, 93, (UINT32)g_width - 18, 93, COLOR_LINE);

    int visible_rows = ((int)g_height - TASKMGR_TABLE_Y - 56) / TASKMGR_ROW_H;
    if (visible_rows < 1) visible_rows = 1;
    int visible = (int)g_task_count - g_scroll_top;
    if (visible > visible_rows) visible = visible_rows;
    if (visible < 0) visible = 0;
    for (int row = 0; row < visible; ++row)
    {
        draw_task_row(row, g_scroll_top + row);
    }

    xapi_DrawSWText(g_window, 20, (UINT32)g_height - 34, g_status, COLOR_MUTED);
    xapi_RefreshWindow(g_window);
    g_need_redraw = false;
}

static bool in_rect(UINT64 x, UINT64 y, int x1, int y1, int x2, int y2)
{
    return x >= (UINT64)x1 && x <= (UINT64)x2 && y >= (UINT64)y1 && y <= (UINT64)y2;
}

static void end_selected_process()
{
    if (!selected_process_can_end())
    {
        g_language = xj380_read_language();
        set_status_text("请先选择一个应用进程。", "Select an app process first.");
        g_need_redraw = true;
        return;
    }

    UINT64 pid = g_tasks[g_selected_index].pid;
    UINT64 ret = xapi_KillProcess(pid);
    char pid_text[24];
    uint64_to_string(pid, pid_text, sizeof(pid_text));

    if (ret == 0)
    {
        strcpy(g_status, taskmgr_tr("已结束 PID ", "Ended PID "));
        strcat(g_status, pid_text);
        strcat(g_status, ".");
        g_selected_index = -1;
        g_selected_pid = 0;
        g_selected_tid = 0;
    }
    else
    {
        strcpy(g_status, taskmgr_tr("结束 PID 失败 ", "Failed to end PID "));
        strcat(g_status, pid_text);
        strcat(g_status, ".");
    }
    load_tasks();
    g_need_redraw = true;
}

static void taskmgr_MessagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    switch (Type)
    {
    case MSG_LBUTTON:
        g_language = xj380_read_language();
        if (in_rect(hData, lData, (int)g_width - 240, 16, (int)g_width - 156, 42))
        {
            load_tasks();
            set_status_text("已刷新。", "Refreshed.");
            g_need_redraw = true;
        }
        else if (in_rect(hData, lData, (int)g_width - 144, 16, (int)g_width - 24, 42))
        {
            end_selected_process();
        }
        else if (lData >= TASKMGR_TABLE_Y && lData < g_height - 48)
        {
            int row = (int)(lData - TASKMGR_TABLE_Y) / TASKMGR_ROW_H;
            int index = g_scroll_top + row;
            if (index >= 0 && index < (int)g_task_count)
            {
                g_selected_index = index;
                g_selected_pid = g_tasks[index].pid;
                g_selected_tid = g_tasks[index].tid;
                strcpy(g_status, taskmgr_tr("已选择 PID ", "Selected PID "));
                char pid_text[24];
                uint64_to_string(g_selected_pid, pid_text, sizeof(pid_text));
                strcat(g_status, pid_text);
                strcat(g_status, ".");
                g_need_redraw = true;
            }
        }
        break;
    case MSG_ROLLER:
        if ((int)hData > 0) g_scroll_top -= 3;
        else g_scroll_top += 3;
        clamp_scroll();
        g_need_redraw = true;
        break;
    case MSG_RESIZE:
        g_width = hData;
        g_height = lData;
        clamp_scroll();
        g_need_redraw = true;
        break;
    default:
        break;
    }
}

static int taskmgr_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    XWINDOW window;
    g_language = xj380_read_language();
    set_status_text("就绪。", "Ready.");
    window.title = taskmgr_tr("任务管理器", "Task Manager");
    window.width = TASKMGR_WIDTH;
    window.height = TASKMGR_HEIGHT;
    window.sets = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;
    xapi_CreateWindow(&g_window, &window);
    xapi_SetIcon(g_window, "/system/icon/unknowexe.png");
    SetMsgPrcor(g_window, taskmgr_MessagePrcor);

    load_tasks();
    render();

    int tick = 0;
    while (true)
    {
        if (g_need_redraw) render();
        xapi_Sleep(100);
        tick++;
        if (tick >= 15)
        {
            load_tasks();
            g_need_redraw = true;
            tick = 0;
        }
        __asm__ __volatile__("pause");
    }
}

extern "C" int taskmgr_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int taskmgr_main_cpp(int argc, char *argv[], char *envp[])
{
    return taskmgr_main_impl(argc, argv, envp);
}
