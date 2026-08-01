// 版权所有©XINGJI Studios 2017-2026 保留所有权利。
// XJ380 类型定义头文件
#ifndef _STDINT_H_
#define _STDINT_H_

// 规范类型
// 无符号整型
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef uint64_t           u64;
typedef uint16_t           u16;
typedef uint32_t           u32;
typedef uint8_t            u8;
#ifdef __UINTPTR_TYPE__
typedef __UINTPTR_TYPE__ uintptr_t;
#endif
// 有符号整型
typedef char      int8_t;
typedef short     int16_t;
typedef int       int32_t;
typedef long long int64_t;
typedef char      CHAR8;
#ifdef __INTPTR_TYPE__
typedef __INTPTR_TYPE__ intptr_t;
#endif
// 浮点型
typedef float  float32_t;
typedef double float64_t;

// 自定义类型
// 其他的在efi.h里
// 浮点型
typedef float32_t f32;
typedef float64_t f64;

#define NULL 0

#ifndef __cplusplus
#    define bool  _Bool
#    define true  1
#    define false 0
#endif

typedef __SIZE_TYPE__ size_t; // compatible to ELF toolchain
typedef int           ssize_t;

typedef union ptr_cast
{
    void     *ptr;
    uintptr_t val;
} ptr_cast_t;

#endif