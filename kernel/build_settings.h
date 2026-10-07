/*
 * 
 *  XJ380 编译设置头文件
 *  Copyright(C) XINGJI Interactive Software 2017-2026 All rights reserved.
 *
 */

/*

    这是一个冰箱，用来防止代码隔夜变质导致出现玄学bug的情况

*/

/* 

⣿⣿⣿⠟⠛⠛⠻⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡟⢋⣩⣉⢻⣿⣿
⣿⣿⣿⠀⣿⣶⣕⣈⠹⠿⠿⠿⠿⠟⠛⣛⢋⣰⠣⣿⣿⠀⣿⣿
⣿⣿⣿⡀⣿⣿⣿⣧⢻⣿⣶⣷⣿⣿⣿⣿⣿⣿⠿⠶⡝⠀⣿⣿
⣿⣿⣿⣷⠘⣿⣿⣿⢏⣿⣿⣋⣀⣈⣻⣿⣿⣷⣤⣤⣿⡐⢿⣿
⣿⣿⣿⣿⣆⢩⣝⣫⣾⣿⣿⣿⣿⡟⠿⠿⠦⠀⠸⠿⣻⣿⡄⢻
⣿⣿⣿⣿⣿⡄⢻⣿⣿⣿⣿⣿⣿⣿⣿⣶⣶⣾⣿⣿⣿⣿⠇⣼
⣿⣿⣿⣿⣿⣿⡄⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡟⣰⣿
⣿⣿⣿⣿⣿⣿⠇⣼⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⢀⣿⣿
⣿⣿⣿⣿⣿⠏⢰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⢸⣿⣿
⣿⣿⣿⣿⠟⣰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⣿⣿
⣿⣿⣿⠋⣴⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡄⣿⣿
⣿⣿⠋⣼⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⢸⣿
             POPO猫
          用于辟邪的小猫
 
*/

#pragma once

#ifndef _BUILD_SETTINGS_H_
#define _BUILD_SETTINGS_H_

#if __has_include("build_config.h")
#include "build_config.h"
#endif

// 编译版本类型
#define DEBUG_VERSION 0
#define RELEASE_VERSION 1

#define CONFIG_OPEN_SERIAL_PRINT

#ifndef CONFIG_OS_EDITION
// Edition label exposed by the boot screen and system version interfaces.
#define CONFIG_OS_EDITION "BetaVersion"
#endif

#ifndef CONFIG_KN_VERSION
// Kernel release string reported by the bootloader/kernel.
#define CONFIG_KN_VERSION "XSK 2.1.0"
#endif

#ifndef CONFIG_OS_VERSION
// User-visible operating system version string.
#define CONFIG_OS_VERSION "XINGJI XJ380 Singularity 1.0.0"
#endif

#ifndef CONFIG_STACK_SIZE
// Default stack size in bytes for kernel-created thread contexts.
#define CONFIG_STACK_SIZE 262144
#endif

#ifndef CONFIG_KERNEL_TASK_STACK_SIZE
// Kernel stack size in bytes for kernel tasks.
#define CONFIG_KERNEL_TASK_STACK_SIZE 1048576
#endif

#ifndef CONFIG_USER_STACK_SIZE
// Initial user stack size in bytes for user processes.
#define CONFIG_USER_STACK_SIZE 16777216
#endif

#ifndef CONFIG_MAX_CPU_NUM
// Maximum CPU slots reserved in per-CPU/SMP data structures.
#define CONFIG_MAX_CPU_NUM 256
#endif

#ifndef CONFIG_KERNEL_HEAP_START
// Virtual base address of the kernel heap.
#define CONFIG_KERNEL_HEAP_START 0xffffc00000000000UL
#endif

#ifndef CONFIG_KERNEL_HEAP_SIZE
// Initial kernel heap capacity in MiB.
#define CONFIG_KERNEL_HEAP_SIZE 256
#endif

#ifndef CONFIG_USER_MMAP_START
// Base address used for user mmap allocations.
#define CONFIG_USER_MMAP_START 0x0000400000000000UL
#endif

#ifndef CONFIG_USER_BRK_START
// Initial user brk address for process data/heap growth.
#define CONFIG_USER_BRK_START 0x0000700000000000UL
#endif

#ifndef CONFIG_USER_BRK_END
// Upper bound of the user brk region.
#define CONFIG_USER_BRK_END 0x00007ffff0000000UL
#endif

#ifndef CONFIG_USER_ELF_HEADER_START
// User virtual address where the ELF image/header mapping begins.
#define CONFIG_USER_ELF_HEADER_START 0x0000300000000000UL
#endif

#ifndef CONFIG_KERNEL_DEFAULT_USER_APP
// User application launched automatically when auto-start is enabled.
#define CONFIG_KERNEL_DEFAULT_USER_APP "/apps/system/shell.elf"
#endif

#ifndef CONFIG_KERNEL_BOOT_LOGO
#define CONFIG_KERNEL_BOOT_LOGO 1
#endif

#ifndef CONFIG_KERNEL_START_DESKTOP
#define CONFIG_KERNEL_START_DESKTOP 1
#endif

#ifndef CONFIG_KERNEL_START_COMPONENT_FLUSHER
#define CONFIG_KERNEL_START_COMPONENT_FLUSHER 1
#endif

#ifndef CONFIG_KERNEL_AUTO_START_USER_APP
#define CONFIG_KERNEL_AUTO_START_USER_APP 1
#endif

#ifndef CONFIG_KERNEL_LOAD_MODULES
#define CONFIG_KERNEL_LOAD_MODULES 1
#endif

#ifndef CONFIG_KERNEL_ENABLE_PROCESS_KILLER
#define CONFIG_KERNEL_ENABLE_PROCESS_KILLER 0
#endif

#ifndef CONFIG_KERNEL_AHCI_QEMU_ACCEL
#define CONFIG_KERNEL_AHCI_QEMU_ACCEL 1
#endif

#ifndef CONFIG_KERNEL_BUSYBOX_ALIASES
#define CONFIG_KERNEL_BUSYBOX_ALIASES 1
#endif

/* High-volume diagnostics are opt-in: synchronous serial output can distort timing. */
#ifndef CONFIG_KERNEL_DEBUG_SCHEDULER_FMANAGER_TIMER_LOG
// Sample scheduler state when fmanager is runnable (default: off).
#define CONFIG_KERNEL_DEBUG_SCHEDULER_FMANAGER_TIMER_LOG 0
#endif

#ifndef CONFIG_KERNEL_DEBUG_GRAPHICS_SYSCALL_TIMING
// Log slow graphics syscall durations (default: off).
#define CONFIG_KERNEL_DEBUG_GRAPHICS_SYSCALL_TIMING 0
#endif

#ifndef CONFIG_KERNEL_DEBUG_MOUSE_LOOP
// Log sampled mouse-loop state (default: off).
#define CONFIG_KERNEL_DEBUG_MOUSE_LOOP 0
#endif

#ifndef CONFIG_KERNEL_DESKTOP_DEBUG_INPUT_ECHO
#define CONFIG_KERNEL_DESKTOP_DEBUG_INPUT_ECHO 1
#endif

#if defined(CONFIG_BUILD_EDITION_RELEASE)
#define BUILD_EDITION RELEASE_VERSION
#else
#define BUILD_EDITION DEBUG_VERSION
#endif

#if defined(CONFIG_OPEN_SERIAL_PRINT)
#define DEVELOP_MODE_SERIAL_OUTPUT 1
#else
#define DEVELOP_MODE_SERIAL_OUTPUT 0
#endif

#define OS_EDITION CONFIG_OS_EDITION
#define KN_VERSION CONFIG_KN_VERSION
#define OS_VERSION CONFIG_OS_VERSION

#define ENVP_SYSTEM_VERSION "SYSTEM_VERSION=" CONFIG_OS_VERSION

#define STACK_SIZE (CONFIG_STACK_SIZE * 1UL)
#define KERNEL_TASK_STACK_SIZE (CONFIG_KERNEL_TASK_STACK_SIZE * 1UL)
#define USER_STACK_SIZE (CONFIG_USER_STACK_SIZE * 1UL)
#define KERNEL_HEAP_BASE CONFIG_KERNEL_HEAP_START
#define KERNEL_HEAP_BYTES (CONFIG_KERNEL_HEAP_SIZE * 1024UL * 1024UL)

#endif
