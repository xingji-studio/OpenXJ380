// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <syscall/syscall.h>
#include <task/pcb.h>
#include <errno.h>

// Kernel-private operation references. The public fd_file_handle layout and
// filesystem/module callbacks remain unchanged.
struct FdOperationRecord;
FdOperationRecord *fd_operation_acquire(int fd, int &error);
// Trusted procfs observer: caller must retain the target PCB for the complete
// acquisition. The returned ticket retains the descriptor independently of
// table replacement/close; it does not itself retain the process object.
FdOperationRecord *fd_operation_acquire_process(pcb_t process, int fd, int &error);
// Snapshot numeric names only. Count is the required capacity; no allocation,
// VFS callback or borrowed handle escapes either descriptor-table lock.
int fd_snapshot_numbers(pcb_t process, size_t *numbers, size_t capacity, size_t &count);
fd_file_handle *fd_operation_handle(FdOperationRecord *record);
void fd_operation_release(FdOperationRecord *record);
int fd_retire_handle(fd_file_handle *handle);
// Publish a prepared handle; no caller may dereference it after success.
int fd_install_handle(pcb_t process, fd_file_handle *handle, int minimum, bool exact,
                      FdOperationRecord **publisher = NULL);
fd_file_handle *fd_detach_descriptor(pcb_t process, size_t fd);
fd_file_handle *fd_detach_descriptor_if(pcb_t process, size_t fd, fd_file_handle *expected);
fd_file_handle *fd_detach_matching(pcb_t process, size_t first, size_t last, bool cloexec);
fd_file_handle *fd_detach_first(lock_queue *table);
void fd_mark_cloexec(pcb_t process, size_t first, size_t last);
lock_queue *fd_detach_process_table(pcb_t process);
// Call only after a dead thread has no current/selected/previous-stack
// scheduler references. False means its operation references must remain intact.
bool fd_operation_thread_exit(tcb_t task);
void fd_reap_abandoned_operations();
void reap_quiesced_file_descriptors();
void process_complete_wait(pcb_t child);
void process_release_wait_claims(tcb_t owner);
void process_reap_wait_claims();

class FdOperation
{
    FdOperationRecord *record_ = NULL;

public:
    FdOperation() = default;
    FdOperation(const FdOperation &) = delete;
    FdOperation &operator=(const FdOperation &) = delete;
    ~FdOperation() { fd_operation_release(record_); }
    int acquire(int fd)
    {
        if (record_) return -EBUSY;
        int error = 0;
        record_ = fd_operation_acquire(fd, error);
        return error;
    }
    int acquire_process(pcb_t process, int fd)
    {
        if (record_) return -EBUSY;
        int error = 0;
        record_ = fd_operation_acquire_process(process, fd, error);
        return error;
    }
    int install(pcb_t process, fd_file_handle *handle, int minimum, bool exact)
    {
        if (record_) return -EBUSY;
        return fd_install_handle(process, handle, minimum, exact, &record_);
    }
    fd_file_handle *get() const { return fd_operation_handle(record_); }
};
