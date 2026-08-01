/*
 *
 *      XINGJI XJ380 Application Compile Tools (XACT) 
 *      XACT C\C++ Header
 *      XJ380API Define Header
 *      Release Version 1.2.0 - 2026/1/16
 *      Copyright(C) XINGJI Interactive Software 2017 - 2026 All rights reserved.
 * 
 *      A XINGJI Interactive Software Production
 * 
 */

#ifndef _XSTDINT_H_
#define _XSTDINT_H_

#ifndef _STDINT_H_
#define _STDINT_H_

#ifdef __cplusplus
extern "C"
{
#endif
#ifdef __cplusplus
}
#endif

// 规范类型
// 无符号整型
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint64_t uintptr_t;
// 有符号整型
typedef char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef int64_t intptr_t;
// 浮点型
typedef float float32_t;
typedef double float64_t;

#define NULL 0

// 检查是否定义
#ifndef __SIZE_T
#define __SIZE_T
typedef unsigned long long int size_t;
#endif

#endif

// efi\Windows风格
// 无符号整型
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
// 有符号整型
typedef int8_t INT8;
typedef int16_t INT16;
typedef int32_t INT32;
typedef int64_t INT64;
// 布尔类型
typedef char BOOLEAN;

// 字符
typedef const char CHAR8;
typedef char* WSTR;

#endif
