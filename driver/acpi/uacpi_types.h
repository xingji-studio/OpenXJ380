#pragma once

#include <stdint.h>

typedef __SIZE_TYPE__ size_t;
#if !defined(__cplusplus) && !defined(bool)
#    define bool  _Bool
#    define true  1
#    define false 0
#endif

// Use the kernel's integer ABI and compiler-provided freestanding varargs.
// This avoids pulling hosted libc headers into either kernel build.
typedef uint8_t     uacpi_u8;
typedef uint16_t    uacpi_u16;
typedef uint32_t    uacpi_u32;
typedef uint64_t    uacpi_u64;
typedef signed char uacpi_i8;
typedef int16_t     uacpi_i16;
typedef int32_t     uacpi_i32;
typedef int64_t     uacpi_i64;
#ifdef __cplusplus
typedef bool uacpi_bool;
#else
typedef _Bool uacpi_bool;
#endif
#define UACPI_TRUE  ((uacpi_bool)1)
#define UACPI_FALSE ((uacpi_bool)0)
#define UACPI_NULL  0
typedef uintptr_t         uacpi_uintptr;
typedef uacpi_uintptr     uacpi_virt_addr;
typedef __SIZE_TYPE__     uacpi_size;
typedef __builtin_va_list uacpi_va_list;
#define uacpi_va_start __builtin_va_start
#define uacpi_va_end   __builtin_va_end
#define uacpi_va_arg   __builtin_va_arg
typedef char uacpi_char;
#define uacpi_offsetof     __builtin_offsetof
#define UACPI_PRIu64       "llu"
#define UACPI_PRIx64       "llx"
#define UACPI_PRIX64       "llX"
#define UACPI_FMT64(value) ((unsigned long long)(value))
