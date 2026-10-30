#pragma once

#include <user/permissions.h>

static constexpr const char *XJ380_FILE_DIALOG_PATH = "/apps/system/filedlg/filedlg.elf";
static constexpr int XJ380_FILE_DIALOG_RESULT_FD = 3;
static constexpr const char *XJ380_TERMINAL_PATH = "/apps/system/shell/shell.elf";

struct ProcessLaunchOptions
{
    ProcessCredentialSource credentials = XPCS_SESSION;
    bool file_dialog = false;
    // Ownership transfers to the launch function even when creation fails.
    vfs_node_t result_writer = NULL;
};

pcb_t create_process_group_with_credentials(const char *name, pcb_t parent, page_directory_t *directory,
                                             char *cmdline, ProcessCredentialSource source);
int create_user_process_with_options(char *path, pcb_t parent, char *argv[], const ProcessLaunchOptions &options);
