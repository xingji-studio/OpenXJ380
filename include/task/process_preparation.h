// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <task/pcb.h>
#include <user/process_launch.h>
struct ProcessPreparation;
ProcessPreparation *process_preparation_acquire(pcb_t process);
void process_preparation_release(ProcessPreparation *pin);
pcb_t create_process_group_prepared(const char *name, pcb_t parent, page_directory_t *directory,
                                     char *cmdline, ProcessCredentialSource source, ProcessPreparation **pin);
class ProcessPreparationGuard
{
    ProcessPreparation *pin;
public:
    explicit ProcessPreparationGuard(ProcessPreparation *prepared) : pin(prepared) {}
    ~ProcessPreparationGuard() { process_preparation_release(pin); }
    ProcessPreparationGuard(const ProcessPreparationGuard &) = delete;
    ProcessPreparationGuard &operator=(const ProcessPreparationGuard &) = delete;
};
