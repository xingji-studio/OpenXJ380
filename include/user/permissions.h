#pragma once

#include <task/pcb.h>

struct PermissionPrompt
{
    uint64_t token;
    size_t pid, tid;
    uint64_t ppid;
    char name[32], path[256], user[64];
    UserType current_type, target_type;
    uint8_t current, requested, asking;
    bool individual;
};

void permissions_session_publish(const UserInfo *info);
enum ProcessCredentialSource { XPCS_SESSION, XPCS_VISITOR, XPCS_INHERIT };
void permissions_process_init(pcb_t process, pcb_t parent, ProcessCredentialSource source = XPCS_INHERIT);
bool permissions_launch_matches(pcb_t process, pcb_t parent, ProcessCredentialSource source);
bool permissions_dialog_metadata(vfs_node_t node, int operation);
void permissions_process_exec(pcb_t process, const char *directory = NULL);
int permissions_executable_directory(vfs_node_t executable, char *out, size_t capacity);
void permissions_thread_exit(tcb_t task);
uint8_t permissions_current_mask();
UserType permissions_current_type();
int permissions_lift(UserType type);
int permissions_request(uint8_t requested);
bool permissions_prompt(PermissionPrompt *out);
bool permissions_modal_active();
void permissions_answer(uint64_t token, bool approved);
void permissions_set_backend(bool available);
bool permissions_have(uint8_t required);
bool permissions_file_access(vfs_node_t node, const char *path, int operation);
bool permissions_file_move(vfs_node_t node, const char *destination);
bool permissions_file_link(vfs_node_t node, const char *destination);

// Future per-directory consent applies only to the NV category. The callback
// runs outside all permission/VFS spin locks; it must not alter mandatory CS/SR/SW rules.
enum PermissionFolderDecision { XFD_DEFAULT, XFD_ALLOW, XFD_DENY, XFD_CONFIRM };
typedef PermissionFolderDecision (*PermissionFolderPolicy)(pcb_t, const char *, int);
void permissions_set_folder_policy(PermissionFolderPolicy policy);
int permissions_file_error();
void permissions_clear_file_error();
void permissions_set_file_error(int error);

// Only authenticated login/profile services may construct this scope. It
// authorizes fixed profile paths, never arbitrary symlink targets or raw disks.
class PermissionProfileScope
{
    tcb_t task;
    const char *previous;
    char owner[64];
public:
    explicit PermissionProfileScope(const char *username);
    ~PermissionProfileScope();
    PermissionProfileScope(const PermissionProfileScope &) = delete;
    PermissionProfileScope &operator=(const PermissionProfileScope &) = delete;
};

// Only the fixed file picker executable may be loaded through this service.
// Its interpreter/dependencies still use the caller's ordinary access policy.
class PermissionDialogLaunchScope
{
    tcb_t task;
public:
    PermissionDialogLaunchScope();
    ~PermissionDialogLaunchScope();
    PermissionDialogLaunchScope(const PermissionDialogLaunchScope &) = delete;
    PermissionDialogLaunchScope &operator=(const PermissionDialogLaunchScope &) = delete;
};

class PermissionDialogReaderScope
{
    tcb_t task;
public:
    explicit PermissionDialogReaderScope(vfs_node_t reader);
    ~PermissionDialogReaderScope();
    PermissionDialogReaderScope(const PermissionDialogReaderScope &) = delete;
    PermissionDialogReaderScope &operator=(const PermissionDialogReaderScope &) = delete;
};

// Console startup execs the fixed shell without adding NV to the application.
// finish() must run before exec's non-returning user-mode handoff.
class PermissionTerminalLaunchScope
{
    tcb_t task;
    bool previous, active;
public:
    explicit PermissionTerminalLaunchScope(bool enabled);
    ~PermissionTerminalLaunchScope();
    void finish();
    PermissionTerminalLaunchScope(const PermissionTerminalLaunchScope &) = delete;
    PermissionTerminalLaunchScope &operator=(const PermissionTerminalLaunchScope &) = delete;
};
