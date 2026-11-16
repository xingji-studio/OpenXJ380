#pragma once

#include <mm/page.h>
#include <stdint.h>

typedef struct thread_control_block *tcb_t;

int futex_wait_user(page_directory_t *directory, uint32_t *address, uint32_t expected,
                    bool private_key, uint32_t bitset, uint64_t deadline);
int futex_wake_user(page_directory_t *directory, uint32_t *address, bool private_key,
                    uint32_t count, uint32_t bitset);
// Called only after a terminal thread has no CPU scheduler references.
void futex_thread_exit(tcb_t task);
