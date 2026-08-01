#pragma once
#include <stdint.h>

typedef struct thread_control_block *tcb_t;

extern const uint64_t TIME_SLICE;
extern const uint64_t MIN_SLICE;

// 调度器函数声明
void scheduler_yield();
void scheduler_sleep_ns(uint64_t nano);
void scheduler_wake_task(tcb_t task);
void scheduler_tick();
void scheduler_init_task(tcb_t task);

// tcb_t select_next_task_safe();
