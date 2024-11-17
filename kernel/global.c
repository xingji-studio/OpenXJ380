#define GLOBAL_VARIABLES_HERE

#include "include.h"

PUBLIC BOOTINFO *binfo = (BOOTINFO *) 0xb02;

PUBLIC PROCESS proc_table[NR_TASKS];
PUBLIC char    task_stack[STACK_SIZE_TOTAL];
//多任务
PUBLIC TASK    task_table[NR_TASKS] = {
    {TestA, STACK_SIZE_TESTA, "TestA"},
    {TestB, STACK_SIZE_TESTB, "TestB"},
    {TestC, STACK_SIZE_TESTC, "TestC"},
};

PUBLIC irq_handler irq_table[NR_IRQ];

PUBLIC system_call sys_call_table[NR_SYS_CALL] = {
    sys_get_ticks,
    sys_write,
};

PUBLIC u32     *buf_back = (u32 *) 0x600000;