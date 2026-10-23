#include "ime_internal.h"
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <ime_utf8.h>
#include <mutex.h>
#include <proto.hpp>
#include <syscall/pxapi.h>
#include <syscall/xapi_user.h>
#include <task/scheduler.h>
#include <user/user.h>

static mutex_t        ime_lock;
static bool           initialized, config_loaded, enabled;
static int            config_error;
static ImeRegistry    registry;
static ImeFocus       focus;
static uint64_t       context_generation = 1, next_sequence = 1, session_generation = 1;
static uint64_t       engine_pid, engine_session, launch_generation                 = 1;
static uint64_t       ui_revision = 1;
static uint32_t       modifiers;
static bool           launch_requested, reset_pending = true;
static XImeUi         ui;
static const size_t   EVENT_CAPACITY   = 128;
static const uint64_t EVENT_TIMEOUT_NS = 2000000000ULL;
struct PendingEvent
{
    XImeEvent event;
    bool      fetched, replied;
    uint32_t  action;
    uint64_t  created;
    char      text[XIME_TEXT_BYTES];
    size_t    text_offset;
};
static PendingEvent events[EVENT_CAPACITY];
static size_t       event_head, event_count;

static void fail_open_locked();

struct ImeGuard
{
    ImeGuard()
    {
        bool abandoned = false;
        mutex_lock_recoverable(&ime_lock, &abandoned);
        if (abandoned)
        {
            // The dead caller may have stopped during a configuration write.
            // Reload the verified journal and revoke partially handled events.
            config_loaded = false;
            fail_open_locked();
        }
    }
    ~ImeGuard()
    {
        mutex_unlock(&ime_lock);
    }
};

static void reset_locked()
{
    ++context_generation;
    event_head = event_count = 0;
    memset(&ui, 0, sizeof(ui));
    ui.size    = sizeof(ui);
    ui.version = XIME_ABI_VERSION;
    ++ui_revision;
    reset_pending = true;
}

static void detach_locked()
{
    engine_pid = engine_session = 0;
    ++launch_generation;
    reset_locked();
}

static void fail_open_locked()
{
    engine_pid = engine_session = 0;
    ++launch_generation;
    memset(&ui, 0, sizeof(ui));
    ++ui_revision;
    reset_pending = true;
    // Preserve accepted replies, including a partially delivered UTF-8 commit.
    // Only unanswered events are replayed. Replaying a replied key could insert
    // it twice or undo the engine's already accepted decision to consume it.
    for (size_t i = 0; i < event_count; ++i)
    {
        PendingEvent &pending = events[(event_head + i) % EVENT_CAPACITY];
        if (pending.replied) continue;
        pending.replied = true;
        pending.action  = XIME_REPLY_PASS;
    }
}

void ime_initialize()
{
    mutex_create(&ime_lock, false);
    initialized = true;
}

static int find_entry(const char *id)
{
    for (uint32_t i = 0; i < registry.count; ++i)
        if (!strcmp(registry.entries[i].id, id)) return (int)i;
    return -1;
}

static void load_locked()
{
    if (config_loaded) return;
    config_loaded = true;
    config_error  = ime_config_load(&registry);
    if (config_error) return;
    // Provision the bundled engine once. A deliberately empty active ID in an
    // existing configuration remains direct input on subsequent boots.
    if (!registry.provisioned)
    {
        vfs_node_t file = vfs_open("/apps/system/inmeth_pinyin.elf");
        if (file)
        {
            vfs_close(file);
            registry.provisioned = true;
            registry.count       = 1;
            strcpy(registry.entries[0].id, "xingji.pinyin");
            strcpy(registry.entries[0].name, "星际拼音");
            strcpy(registry.entries[0].executable, "/apps/system/inmeth_pinyin.elf");
            strcpy(registry.active, "xingji.pinyin");
            config_error = ime_config_save(&registry);
        }
    }
    launch_requested = registry.active[0] != 0 && !config_error;
}

void ime_process_loaded(pcb_t process)
{
    tcb_t current = get_current_task();
    if (!process) return;
    process->ime_role              = current ? current->ime_launch_role : 0;
    process->ime_launch_generation = current ? current->ime_launch_generation : 0;
}

int ime_launch_manager(pcb_t parent, char **argv)
{
    tcb_t task = get_current_task();
    if (!task) return -ESRCH;
    task->ime_launch_role = 2;
    int result            = create_user_process_from_file((char *)"/apps/system/ctrlmenu.elf", parent, argv);
    task->ime_launch_role = 0;
    return result;
}

void ime_process_exit(pcb_t process)
{
    if (!initialized || !process) return;
    bool     abandoned = mutex_abort_for_process(&ime_lock, process);
    ImeGuard guard;
    if (abandoned)
    {
        config_loaded = false;
        fail_open_locked();
    }
    process->ime_role              = 0;
    process->ime_launch_generation = 0;
    if (engine_pid == process->pid) fail_open_locked();
    if (focus.pid == process->pid)
    {
        memset(&focus, 0, sizeof(focus));
        reset_locked();
    }
}

void ime_service()
{
    if (!initialized) return;
    char     path[XIME_PATH_BYTES] = {};
    uint64_t generation            = 0;
    {
        ImeGuard guard;
        if (!enabled) return;
        load_locked();
        if (config_error) return;
        uint64_t now = nanoTime();
        if (engine_session && event_count && now - events[event_head].created > EVENT_TIMEOUT_NS)
        {
            // Stop giving new keys to a stalled engine. Already queued original
            // messages are replayed, in order, by the graphics worker below.
            fail_open_locked();
        }
        if (launch_requested && !engine_pid)
        {
            launch_requested = false;
            int entry        = find_entry(registry.active);
            if (entry >= 0)
            {
                strcpy(path, registry.entries[entry].executable);
                generation = launch_generation;
            }
        }
    }
    if (path[0])
    {
        tcb_t task                  = get_current_task();
        task->ime_launch_role       = 1;
        task->ime_launch_generation = generation;
        int pid                     = create_user_process_from_file(path, NULL, NULL);
        task->ime_launch_role       = 0;
        task->ime_launch_generation = 0;
        ImeGuard guard;
        if (pid < 0)
        {
            detach_locked();
            write_serial_fmt("ime: launch failed %d\n", pid);
        }
    }
}

void ime_set_focus(const ImeFocus *next)
{
    if (!initialized) return;
    ImeGuard guard;
    ImeFocus empty = {};
    if (!next) next = &empty;
    bool changed = next->pid != focus.pid || next->tid != focus.tid || next->window != focus.window ||
                   next->control != focus.control || next->flags != focus.flags || next->callback != focus.callback;
    bool moved = next->x != focus.x || next->y != focus.y;
    focus      = *next;
    // Graphics calls this only after login is complete for enabled text fields.
    if (focus.flags & XIME_CONTEXT_ENABLED) enabled = true;
    if (changed)
        reset_locked();
    else if (moved)
        ++ui_revision;
}

void ime_set_modifiers(uint32_t value)
{
    if (!initialized) return;
    ImeGuard guard;
    modifiers = value;
}

void ime_reset_input()
{
    if (!initialized) return;
    ImeGuard guard;
    reset_locked();
}

static bool queue_event(uint32_t kind, uint64_t type, uint64_t value, bool reserve = false)
{
    // Keep one slot for the key that triggers fallback. The graphics worker
    // drains ready deliveries after each message, before dispatching another.
    if (event_count >= EVENT_CAPACITY - (reserve ? 0 : 1)) return false;
    PendingEvent &pending = events[(event_head + event_count++) % EVENT_CAPACITY];
    memset(&pending, 0, sizeof(pending));
    pending.created  = nanoTime();
    XImeEvent &event = pending.event;
    event.size       = sizeof(event);
    event.version    = XIME_ABI_VERSION;
    event.session    = engine_session;
    event.sequence   = next_sequence++;
    event.context    = context_generation;
    event.kind       = kind;
    event.modifiers  = modifiers;
    event.mode       = registry.mode;
    event.message    = type;
    event.value      = value;
    return true;
}

bool ime_filter_message(uint64_t type, uint64_t value, uint64_t callback, tcb_t task)
{
    if (!initialized || !task || !task->parent_group) return false;
    if (type != MSG_KEYDOWN && type != MSG_KEYUP && type != MSG_KEYLONGPRESS && type != MSG_CHAR && type != MSG_SPCHAR)
        return false;
    ImeGuard guard;
    if (!engine_session || task->parent_group->pid != focus.pid || task->tid != focus.tid ||
        callback != focus.callback || !(focus.flags & XIME_CONTEXT_ENABLED) || (focus.flags & XIME_CONTEXT_PASSWORD))
        return false;
    bool toggle = (modifiers & XIME_MOD_CTRL) && !(modifiers & (XIME_MOD_ALT | XIME_MOD_WIN)) && value == ' ';
    if (!toggle && (modifiers & (XIME_MOD_CTRL | XIME_MOD_ALT | XIME_MOD_WIN)))
    {
        if (ui.visible) reset_locked();
        return false;
    }
    if (!toggle && registry.mode == XIME_MODE_ENGLISH && !ui.visible && !event_count) return false;
    if (queue_event(XIME_EVENT_KEY, type, value)) return true;
    fail_open_locked();
    queue_event(XIME_EVENT_KEY, type, value, true);
    PendingEvent &pending = events[(event_head + event_count - 1) % EVENT_CAPACITY];
    pending.replied       = true;
    pending.action        = XIME_REPLY_PASS;
    return true;
}

bool ime_take_delivery(ImeDelivery *delivery)
{
    if (!initialized || !delivery) return false;
    ImeGuard guard;
    while (event_count)
    {
        PendingEvent &pending = events[event_head];
        if (!pending.replied) return false;
        if (pending.event.context != context_generation) pending.action = XIME_REPLY_CONSUMED;
        delivery->context = context_generation;
        delivery->target  = focus;
        if (pending.action == XIME_REPLY_COMMIT_UTF8 && pending.text[pending.text_offset])
        {
            const char *s        = pending.text + pending.text_offset;
            size_t      n        = ime_utf8_scalar_bytes(s, XIME_TEXT_BYTES - pending.text_offset);
            delivery->message    = MSG_CHAR;
            delivery->value      = ime_utf8_pack(s, n);
            pending.text_offset += n;
            return true;
        }
        bool pass         = pending.action == XIME_REPLY_PASS && pending.event.kind == XIME_EVENT_KEY;
        delivery->message = pending.event.message;
        delivery->value   = pending.event.value;
        event_head        = (event_head + 1) % EVENT_CAPACITY;
        --event_count;
        if (pass) return true;
    }
    return false;
}

bool ime_snapshot_ui(XImeUi *out, int *x, int *y, uint64_t *revision)
{
    if (!initialized) return false;
    ImeGuard guard;
    if (*revision == ui_revision) return false;
    *out      = ui;
    *x        = focus.x;
    *y        = focus.y;
    *revision = ui_revision;
    return true;
}

void ime_candidate_click(uint32_t index)
{
    if (!initialized) return;
    ImeGuard guard;
    if (engine_session && ui.visible && index < ui.count) queue_event(XIME_EVENT_CANDIDATE, 0, index);
}

template <class T>
static int read_request(uint64_t address, T *request)
{
    if (!address || !copy_from_user_pagedir(xapi_current_pagedir(), request, (void *)address, sizeof(T)))
        return -EFAULT;
    if (request->size != sizeof(T) || request->version != XIME_ABI_VERSION) return -EINVAL;
    return 0;
}

template <class T>
static int write_result(uint64_t address, const T *result)
{
    return copy_to_user_pagedir(xapi_current_pagedir(), (void *)address, result, sizeof(T)) ? 0 : -EFAULT;
}

static bool engine_authorized(pcb_t caller)
{
    return caller && caller->ime_role == 1 && caller->ime_launch_generation == launch_generation &&
           caller->pid == engine_pid && engine_session;
}

int64_t ime_syscall(uint64_t number, uint64_t address)
{
    if (!initialized) return -ENODEV;
    tcb_t task = get_current_task();
    if (!task || !task->parent_group) return -ESRCH;
    pcb_t    caller = task->parent_group;
    ImeGuard guard;
    load_locked();
    if (config_error) return config_error;
    bool manager = caller->ime_role == 2;
    switch (number)
    {
    case XAPI_IME_REGISTER: {
        XImeRegistration request = {};
        int              result  = read_request(address, &request);
        if (result) return result;
        if (!ime_valid_id(request.id) || !request.name[0] || !ime_utf8_valid(request.name, sizeof(request.name)) ||
            !caller->exe_path || strlen(caller->exe_path) >= XIME_PATH_BYTES)
            return -EINVAL;
        char *path = normalize_path(caller->exe_path);
        if (!path || path[0] != '/')
        {
            free(path);
            return -EINVAL;
        }
        int existing = find_entry(request.id);
        if (existing >= 0)
        {
            bool same = !strcmp(registry.entries[existing].executable, path) &&
                        !strcmp(registry.entries[existing].name, request.name);
            free(path);
            return same ? 0 : -EEXIST;
        }
        if (registry.count == XIME_MAX_INSTALLED)
        {
            free(path);
            return -ENOSPC;
        }
        // One registration per executable prevents a single app filling the registry.
        for (uint32_t i = 0; i < registry.count; ++i)
            if (!strcmp(registry.entries[i].executable, path))
            {
                free(path);
                return -EEXIST;
            }
        XImeInfo &entry = registry.entries[registry.count++];
        memset(&entry, 0, sizeof(entry));
        strcpy(entry.id, request.id);
        strcpy(entry.name, request.name);
        strcpy(entry.executable, path);
        free(path);
        result = ime_config_save(&registry);
        if (result)
        {
            --registry.count;
            memset(&entry, 0, sizeof(entry));
        }
        return result;
    }
    case XAPI_IME_ENUM: {
        XImeList result = {};
        int      error  = read_request(address, &result);
        if (error) return error;
        memset(&result, 0, sizeof(result));
        result.size    = sizeof(result);
        result.version = XIME_ABI_VERSION;
        result.count   = registry.count;
        memcpy(result.entries, registry.entries, registry.count * sizeof(XImeInfo));
        return write_result(address, &result);
    }
    case XAPI_IME_GET_STATE: {
        XImeState result = {};
        int       error  = read_request(address, &result);
        if (error) return error;
        memset(&result, 0, sizeof(result));
        result.size     = sizeof(result);
        result.version  = XIME_ABI_VERSION;
        result.mode     = registry.mode;
        result.attached = engine_session != 0;
        strcpy(result.active, registry.active);
        return write_result(address, &result);
    }
    case XAPI_IME_SET_ACTIVE:
    case XAPI_IME_UNREGISTER: {
        if (!manager) return -EPERM;
        XImeSelect request = {};
        int        error   = read_request(address, &request);
        if (error) return error;
        if (request.id[0] && !ime_valid_id(request.id)) return -EINVAL;
        int entry = find_entry(request.id);
        if (entry < 0 && (request.id[0] || number == XAPI_IME_UNREGISTER)) return -ENOENT;
        ImeRegistry previous = registry;
        if (number == XAPI_IME_UNREGISTER)
        {
            if (!strcmp(registry.active, request.id)) registry.active[0] = 0;
            for (uint32_t i = (uint32_t)entry + 1; i < registry.count; ++i)
                registry.entries[i - 1] = registry.entries[i];
            memset(&registry.entries[--registry.count], 0, sizeof(XImeInfo));
        }
        else
            strcpy(registry.active, request.id);
        error = ime_config_save(&registry);
        if (error)
        {
            registry = previous;
            return error;
        }
        detach_locked();
        launch_requested = registry.active[0] != 0;
        return 0;
    }
    case XAPI_IME_ATTACH: {
        XImeSession request = {};
        int         error   = read_request(address, &request);
        if (error) return error;
        int entry = find_entry(registry.active);
        if (caller->ime_role != 1 || caller->ime_launch_generation != launch_generation || entry < 0 ||
            !caller->exe_path || strcmp(caller->exe_path, registry.entries[entry].executable))
            return -EPERM;
        if (engine_session) return -EBUSY;
        uint64_t session = ++session_generation;
        request.session  = session;
        error            = write_result(address, &request);
        if (error) return error;
        engine_pid     = caller->pid;
        engine_session = session;
        reset_locked();
        return 0;
    }
    case XAPI_IME_GET_EVENT: {
        if (!engine_authorized(caller)) return -EPERM;
        XImeEvent result = {};
        int       error  = read_request(address, &result);
        if (error) return error;
        if (reset_pending)
        {
            memset(&result, 0, sizeof(result));
            result.size    = sizeof(result);
            result.version = XIME_ABI_VERSION;
            result.kind    = XIME_EVENT_RESET;
            result.session = engine_session;
            result.context = context_generation;
            result.mode    = registry.mode;
            error          = write_result(address, &result);
            if (!error) reset_pending = false;
            return error;
        }
        for (size_t i = 0; i < event_count; ++i)
        {
            PendingEvent &pending = events[(event_head + i) % EVENT_CAPACITY];
            if (pending.replied) continue;
            if (pending.fetched) return -EAGAIN; // Exactly one outstanding reply.
            error = write_result(address, &pending.event);
            if (!error) pending.fetched = true;
            return error;
        }
        return -EAGAIN;
    }
    case XAPI_IME_REPLY_EVENT: {
        if (!engine_authorized(caller)) return -EPERM;
        XImeReply reply = {};
        int       error = read_request(address, &reply);
        if (error) return error;
        if (reply.reserved || reply.action > XIME_REPLY_COMMIT_UTF8 || reply.session != engine_session ||
            reply.context != context_generation || !ime_utf8_valid(reply.text, sizeof(reply.text)))
            return -EINVAL;
        for (const unsigned char *p = (const unsigned char *)reply.text; *p; ++p)
            if (*p < 32 || *p == 127) return -EINVAL;
        for (size_t i = 0; i < event_count; ++i)
        {
            PendingEvent &pending = events[(event_head + i) % EVENT_CAPACITY];
            if (pending.replied) continue;
            if (!pending.fetched || pending.event.sequence != reply.sequence) return -ESTALE;
            pending.action = reply.action;
            memcpy(pending.text, reply.text, sizeof(pending.text));
            pending.replied = true;
            return 0;
        }
        return -ESTALE;
    }
    case XAPI_IME_UPDATE_UI: {
        if (!engine_authorized(caller)) return -EPERM;
        XImeUi value = {};
        int    error = read_request(address, &value);
        if (error) return error;
        if (value.session != engine_session || value.context != context_generation || value.visible > 1 ||
            value.count > XIME_MAX_CANDIDATES || (value.count && value.selected >= value.count) ||
            !ime_utf8_valid(value.preedit, sizeof(value.preedit)))
            return -EINVAL;
        bool outstanding = false;
        for (size_t i = 0; i < event_count; ++i)
        {
            PendingEvent &pending = events[(event_head + i) % EVENT_CAPACITY];
            if (pending.fetched && !pending.replied && pending.event.sequence == value.sequence) outstanding = true;
        }
        if (!outstanding) return -ESTALE;
        for (uint32_t i = 0; i < value.count; ++i)
            if (!ime_utf8_valid(value.candidates[i], XIME_CANDIDATE_BYTES)) return -EINVAL;
        ui = value;
        ++ui_revision;
        return 0;
    }
    case XAPI_IME_SET_MODE: {
        XImeMode value = {};
        int      error = read_request(address, &value);
        if (error) return error;
        if (!manager && (!engine_authorized(caller) || value.session != engine_session)) return -EPERM;
        if (value.reserved || value.mode > XIME_MODE_CHINESE) return -EINVAL;
        uint32_t previous = registry.mode;
        registry.mode     = value.mode;
        error             = ime_config_save(&registry);
        if (error) registry.mode = previous;
        if (!error && manager) reset_locked();
        return error;
    }
    default: return -ENOSYS;
    }
}

void ime_enable()
{
    if (!initialized) return;
    ImeGuard guard;
    enabled = true;
}
