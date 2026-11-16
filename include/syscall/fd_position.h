// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <syscall/syscall.h>
#include <task/pcb.h>

struct FdSharedPosition;

// Private sidecars preserve the public descriptor and filesystem/module ABI.
// Callers retain handles with FdOperation until every position scope ends.
int fd_position_share(fd_file_handle *source, fd_file_handle *copy);
void fd_position_forget(fd_file_handle *handle);
// Called with FD operation references still pinned, before a quiescent TCB is freed.
void fd_position_abandon(tcb_t owner);

class FdPositionScope
{
    fd_file_handle *handles_[2];
    FdSharedPosition *positions_[2] = {};
    bool commit_[2];
    int error_;

public:
    explicit FdPositionScope(fd_file_handle *handle, bool commit = true);
    FdPositionScope(fd_file_handle *first, bool first_commit, fd_file_handle *second, bool second_commit);
    ~FdPositionScope();
    FdPositionScope(const FdPositionScope &) = delete;
    FdPositionScope &operator=(const FdPositionScope &) = delete;
    int error() const { return error_; }
};
