#pragma once

#include <task/pcb.h>

// Requests and operation admission share one short metadata gate. A dying owner
// stays schedulable until its protected state and caller-owned resources unwind.
void  task_request_termination(tcb_t task);
bool  process_request_termination(pcb_t process, int exit_code, bool zombie);
bool  process_has_kernel_operations(pcb_t process);
bool  process_termination_requested(pcb_t process);
tcb_t kernel_operation_enter();
void  kernel_operation_leave(tcb_t task);
tcb_t kernel_boundary_enter();
void  kernel_boundary_leave(tcb_t task);

class KernelOperationScope {
    tcb_t task;

  public:
    KernelOperationScope() : task(kernel_operation_enter()) {}
    ~KernelOperationScope()
    {
        kernel_operation_leave(task);
    }
    KernelOperationScope(const KernelOperationScope &)            = delete;
    KernelOperationScope &operator=(const KernelOperationScope &) = delete;
};

class KernelBoundaryScope {
    tcb_t task;

  public:
    KernelBoundaryScope() : task(kernel_boundary_enter()) {}
    ~KernelBoundaryScope()
    {
        finish();
    }
    void finish()
    {
        tcb_t owner = task;
        task        = NULL;
        kernel_boundary_leave(owner);
    }
    KernelBoundaryScope(const KernelBoundaryScope &)            = delete;
    KernelBoundaryScope &operator=(const KernelBoundaryScope &) = delete;
};
