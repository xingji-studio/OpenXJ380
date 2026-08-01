#pragma once

#define MAX_KERNEL_MODULE 256

#include "krlibc.h"
#include <proto.hpp>
#include <elf.h>
typedef struct {
    bool     is_use;
    char     module_name[64];
    char    *path;
    uint8_t *data;
    size_t   size;
    char     raw_name[64];
} cp_module_t;

void module_setup();
cp_module_t *get_module(const char *module_name);
cp_module_t *get_module_raw(const char *module_name);

#define EXPORT_SYMBOL(name)                                                                        \
    __attribute__((used, section(".ksymtab"))) static const dlfunc_t __ksym_##name = {             \
        #name, (void *)name}

#define EXPORT_SYMBOL_F(func_name, name)                                                           \
    __attribute__((used, section(".ksymtab"))) static const dlfunc_t __ksym_##name = {             \
        #func_name, (void *)name}

typedef int (*dlinit_t)(void);

typedef struct {
    char *name;
    void *addr;
} dlfunc_t;

#define RTLD_LAZY       0x00001     // 延迟绑定（懒加载）
#define RTLD_NOW        0x00002     // 立即绑定
#define RTLD_BINDING_MASK 0x00003   // 绑定模式掩码

// GNU 扩展标志
#define RTLD_GLOBAL     0x00100     // 符号全局可见
#define RTLD_LOCAL      0x00000     // 符号局部可见（默认）
#define RTLD_NODELETE   0x01000     // 不卸载库
#define RTLD_NOLOAD     0x00004     // 不加载，仅检查是否已加载
#define RTLD_DEEPBIND   0x00008     // 优先在自身查找符号

// 内核特定标志
#define RTLD_KERNEL     0x10000     // 加载为内核模块
#define RTLD_USER       0x20000     // 加载为用户态库

// 组合标志
#define RTLD_DEFAULT    ((void*)0)  // 默认查找顺序
#define RTLD_NEXT       ((void*)-1) // 查找下一个出现的符号

typedef struct kernel_mode {
    cp_module_t *module;
    dlinit_t     entry;
    dlinit_t     task_entry;
    int          entry_exit_code;
} kernel_mode_t;

/**
 * 加载一个内核模块
 * @param kmod 内核模块管理单元
 * @param module 文件句柄
 */
void dlinker_load(kernel_mode_t *kmod, cp_module_t *module);

dlfunc_t *find_func(const char *name);

void find_kernel_symbol();

void dlinker_init();

void load_all_kernel_module();

void start_all_kernel_module();

void module_setup();
// 动态库句柄
typedef struct dlhandle {
    char*           path;           // 动态库路径
    uint64_t        base_addr;      // 加载基地址
    void*           ehdr;           // ELF 头指针
    size_t          size;           // 动态库大小
    struct dlhandle* next;          // 链表下一个
    struct dlhandle* prev;          // 链表上一个
    bool            is_kernel;      // 是否是内核模块
    page_directory_t* pd;           // 所属页目录（用户态）
    
    // 动态段信息
    Elf64_Sym*      symtab;
    char*           strtab;
    size_t          symtabsz;
    size_t          sym_count;
    Elf64_Dyn*      dynamic;
    size_t          ref_count;
    bool            init_called;
    
    // 导出符号表
    dlfunc_t*       exports;
    size_t          export_count;
    
    // 依赖项
    struct dlhandle** dependencies;
    size_t           dep_count;

    // 搜索路径
    char            *rpath;
} dlhandle_t;
dlhandle_t* load_shared_object_internal(const char* path, int flags, 
                                               page_directory_t* pd);

bool process_dynamic_relocations(Elf64_Dyn* dyn, dlhandle_t* handle);

#ifdef __cplusplus
extern "C" {
#endif

// 标准动态链接接口
void* dlopen(const char* filename, int flags);
void* dlsym(void* handle, const char* symbol);
int dlclose(void* handle);
char* dlerror(void);

#ifdef __cplusplus
}
#endif
