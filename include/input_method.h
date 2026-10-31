#pragma once
#include <task/pcb.h>
#include <user/permissions.h>
#include <xime.h>

// Core owns the session; graphics owns and validates window lifetimes.
struct ImeFocus
{
    uint64_t pid, tid, window, control, callback;
    uint32_t flags;
    int32_t  x, y;
};
struct ImeDelivery
{
    uint64_t context, message, value;
    ImeFocus target;
};
struct ImeModeNotice
{
    uint64_t revision;
    uint32_t mode;
    int32_t  x, y;
};
bool    ime_snapshot_mode_notice(ImeModeNotice *notice);
void    ime_initialize();
void    ime_service();
void    ime_set_focus(const ImeFocus *focus);
void    ime_set_modifiers(uint32_t modifiers);
void    ime_reset_input();
bool    ime_filter_message(uint64_t type, uint64_t value, uint64_t callback, tcb_t task);
bool    ime_take_delivery(ImeDelivery *delivery);
bool    ime_snapshot_ui(XImeUi *ui, int *x, int *y, uint64_t *revision);
void    ime_candidate_click(uint32_t index);
bool    ime_snapshot_ui_v2(XImeUiV2 *ui, int *x, int *y, uint64_t *revision);
void    ime_ui_action(uint32_t kind, uint64_t value, uint64_t revision, uint64_t context);
int64_t ime_syscall(uint64_t number, uint64_t user_request);
void    ime_process_loaded(pcb_t process);
void    ime_process_exit(pcb_t process);
int     ime_launch_manager(pcb_t parent, char **argv, ProcessCredentialSource source = XPCS_SESSION);

// This is a kernel-only policy boundary, shared by legacy XAPI and POSIX I/O.
enum ImeFileOperation
{
    IME_FILE_READ,
    IME_FILE_WRITE,
    IME_FILE_REMOVE,
    IME_FILE_MOUNT,
    IME_FILE_LOOKUP,
    IME_FILE_LIST,
    IME_FILE_STAT,
    IME_FILE_READ_WRITE,
    IME_FILE_EXPORT
};
bool ime_vfs_access(vfs_node_t node, const char *path, ImeFileOperation operation);
bool ime_vfs_requirements(vfs_node_t node, const char *path, ImeFileOperation operation, uint8_t *required);
// Called by FAT after resolving a long/short filename, before opening data.
void ime_vfs_resolved_name(vfs_node_t node, const char *name);

void do_message_unfiltered(uint64_t type, uint64_t high, uint64_t low, void (*callback)(uint64_t, uint64_t, uint64_t),
                           tcb_t task);
void ime_enable();
typedef void (*ImeTextSink)(uint64_t, uint64_t, uint64_t, tcb_t);
void ime_set_text_sink(ImeTextSink sink);
int  message_getch_try(xtttp_dtt *terminal);
