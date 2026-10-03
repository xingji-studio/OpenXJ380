#pragma once
#include <stdint.h>

typedef struct thread_control_block *tcb_t;

extern const uint64_t TIME_SLICE;
extern const uint64_t MIN_SLICE;

// 调度器函数声明
// Global boot/panic gate; runtime critical sections must use local nesting.
void scheduler_start();
void scheduler_stop();
bool scheduler_is_enabled();
// Save/restore only this CPU's nesting, preserving any enclosing section.
// Restore before a non-returning user-mode handoff as well as error returns.
uint64_t scheduler_disable_depth();
void scheduler_restore_depth(uint64_t depth);
void scheduler_yield();
void scheduler_sleep_ns(uint64_t nano);
void scheduler_wake_task(tcb_t task);
void scheduler_tick();
void scheduler_init_task(tcb_t task);

// tcb_t select_next_task_safe();
