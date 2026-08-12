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

#ifndef CONFIG_OS_EDITION
#define CONFIG_OS_EDITION "BetaVersion"
#endif

#ifndef CONFIG_KN_VERSION
#define CONFIG_KN_VERSION "XSK 2.1.0"
#endif

#ifndef CONFIG_OS_VERSION
#define CONFIG_OS_VERSION "XINGJI XJ380 Singularity 1.0.0"
#endif

#ifndef CONFIG_STACK_SIZE
#define CONFIG_STACK_SIZE 262144
#endif

#ifndef CONFIG_KERNEL_TASK_STACK_SIZE
#define CONFIG_KERNEL_TASK_STACK_SIZE 1048576
#endif

#ifndef CONFIG_USER_STACK_SIZE
#define CONFIG_USER_STACK_SIZE 16777216
#endif

#ifndef CONFIG_MAX_CPU_NUM
#define CONFIG_MAX_CPU_NUM 256
#endif

#ifndef CONFIG_KERNEL_HEAP_START
#define CONFIG_KERNEL_HEAP_START 0xffffc00000000000UL
#endif

#ifndef CONFIG_KERNEL_HEAP_SIZE
#define CONFIG_KERNEL_HEAP_SIZE 256
#endif

#ifndef CONFIG_USER_MMAP_START
#define CONFIG_USER_MMAP_START 0x0000400000000000UL
#endif

#ifndef CONFIG_USER_BRK_START
#define CONFIG_USER_BRK_START 0x0000700000000000UL
#endif

#ifndef CONFIG_USER_BRK_END
#define CONFIG_USER_BRK_END 0x00007ffff0000000UL
#endif

#ifndef CONFIG_USER_ELF_HEADER_START
#define CONFIG_USER_ELF_HEADER_START 0x0000300000000000UL
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

#ifndef CONFIG_KERNEL_DESKTOP_DEBUG_INPUT_ECHO
#define CONFIG_KERNEL_DESKTOP_DEBUG_INPUT_ECHO 1
#endif

#if defined(CONFIG_BUILD_EDITION_RELEASE)
#define BUILD_EDITION RELEASE_VERSION
#else
#define BUILD_EDITION DEBUG_VERSION
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
