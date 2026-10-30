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
void permissions_process_init(pcb_t process, pcb_t parent);
void permissions_process_exec(pcb_t process);
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
