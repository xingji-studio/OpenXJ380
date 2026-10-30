#pragma once
#include <stdint.h>

typedef struct thread_control_block *tcb_t;

extern const uint64_t TIME_SLICE;
extern const uint64_t MIN_SLICE;

// 调度器函数声明
/*
 * Scheduler safety contract:
 *
 * - scheduler_start/stop control the system-wide boot or fatal-error gate.
 *   They are not runtime critical-section primitives.
 * - disable_scheduler/enable_scheduler change the current CPU's nesting
 *   depth. Every disable must be paired on every returning path.
 * - Code that temporarily enters a disabled section must save
 *   scheduler_disable_depth() and restore that exact value. Do not blindly
 *   enable once: callers may already be inside an outer disabled section.
 * - Keep interrupts disabled while changing the nesting depth if an interrupt
 *   could observe partially updated state or re-enter protected code.
 * - Never wait, allocate, call VFS, or take a blocking mutex while holding a
 *   spin lock or while scheduling is disabled.
 */
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
