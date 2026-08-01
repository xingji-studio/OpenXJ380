#pragma once

#define TASK_KERNEL_LEVEL      0 // 内核任务 (崩溃后会挂起内核)
#define TASK_IDLE_LEVEL        1 // IDLE (崩溃后与普通内核任务相同行为)
#define TASK_APPLICATION_LEVEL 2 // 应用程序

#include "cpu/fpu.h"
#include "lock_queue.h"
#include "mm/page.h"
#include "stdint.h"
#include "tty.h"
#include "user/user.h"
#include "user/x3tp.h"
#include <mm/vma.h>
#include <syscall/signal.h>

typedef struct registers
{
    uint64_t ds;
    uint64_t es;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t vector;   // 保留
    uint64_t err_code; // 保留
    // CPU自动压入
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} registers_t;

typedef struct process_control_block *pcb_t;
typedef struct thread_control_block  *tcb_t;

typedef enum
{
    CREATE  = 0, // 创建中
    RUNNING = 1, // 运行中
    WAIT    = 2, // 线程阻塞
    DEATH   = 3, // 死亡(无法被调度, 线程状态为等待处死)
    START   = 4, // 准备调度
    FUTEX   = 5, // 被挂起(无法被调度, 线程状态为等待唤醒)
    OUT     = 6, // 已被处死(无法被调度)
    ZOMBIE  = 7,
} TaskStatus;

typedef struct
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t ss, cs, ds, es;
} TaskContext;
struct vfs_node;
typedef struct vfs_node *vfs_node_t;
struct process_control_block
{
    size_t   pid;  // 进程ID
    uint64_t ppid; // 父进程ID

    char              name[32];     // 进程名称
    page_directory_t *pagedir;      // 页表目录
    pcb_t             parent_task;  // 父进程
    lock_queue       *thread_queue; // 线程队列
    size_t            queue_index;  // 进程队列索引
    TaskStatus        status;       // 进程状态

    lock_queue       *ipc_queue;   // 进程消息队列

    lock_queue *file_open; // 文件句柄占用队列
    size_t     *file_open_shared_refs;
    char      **envp;      // 环境变量指针
    size_t      envc;      // 环境变量数量

    bool        vfork;       // 是否是 vfork 创建的进程
    size_t      child_index; // 子进程队列索引
    lock_queue *child_pcb;   // 子进程列表
    int         exit_code;   // 进程退出码
    uint64_t    mmap_start;  // 映射起始地址
    char       *cmdline;
    char      **argv;
    size_t      argc;
    char       *exe_path;
    int         task_level; // 进程等级
    lock_queue *virt_queue; // 虚拟页分配队列
    void       *elf_file;   // 可执行文件指针
    size_t      elf_size;   // 可执行文件大小
    uint64_t    load_start; // 加载起始地址
    uint64_t    aux_phdr;
    uint64_t    aux_phent;
    uint64_t    aux_phnum;
    uint64_t    aux_base;
    uint64_t    aux_entry;
    uint64_t    aux_execfn;
    bool        linux_abi;
    uint16_t    umask;
    vma_manager_t     vma_manager; // VMA 分配管理器
    vfs_node_t        procfs_node; // 进程私有 procfs 文件句柄
    vfs_node_t        proc_root;   // 进程私有根文件节点
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t brk_current ;

    tty_t       *tty;
    xtttp_dtt   *xtttp_stc;

    int      notify_pcor_pipe_read_fd;
    int      notify_pcor_pipe_write_fd;
    size_t   notify_pcor_tid;
    uint64_t notify_pcor_func;
    bool     notify_pcor_registered;
};
#ifndef CONFIG_USER_ELF_HEADER_START
#define CONFIG_USER_ELF_HEADER_START 0x0000300000000000UL
#endif
#ifndef CONFIG_USER_BRK_START
#define CONFIG_USER_BRK_START 0x0000700000000000UL
#endif
#ifndef CONFIG_USER_BRK_END
#define CONFIG_USER_BRK_END 0x00007ffff0000000UL
#endif

#define EHDR_START_ADDR CONFIG_USER_ELF_HEADER_START // ELF头起始地址
uint64_t parse_elf_file(char *path, pcb_t group);
uint64_t process_execve(char *path, char **argv, char **envp);
#define USER_BRK_START CONFIG_USER_BRK_START
#define USER_BRK_END CONFIG_USER_BRK_END

struct thread_control_block
{
    pcb_t parent_group; // 父进程
    int   task_level;   // 进程等级

    size_t        tid;         // 线程ID
    char          name[32];    // 线程名称
    TaskStatus    status;      // 线程状态
    uint64_t      wakeup_time;  // WAIT 线程的唤醒时间(ns)，0 表示不按时间唤醒
    size_t        cpu_id;      // 运行的CPU ID
    TaskContext   context0;    // 线程上下文
    fpu_context_t fpu_context; // 浮点寄存器上下文

    sigaction_t   actions[64]; // 信号处理器回调
    uint64_t      blocked;         // 屏蔽位图
    uint64_t      signal;          // 信号位图
    uint64_t kernel_stack;   // 内核栈地址
    size_t   queue_index;    // 调度队列索引
    lock_node *sched_node;   // 调度队列节点
    size_t   group_index;    // 进程队列索引
    uint64_t main;           // 线程入口地址
    uint64_t user_stack;     // 用户栈
    uint64_t user_stack_top; // 用户栈顶部地址

    uint64_t syscall_stack;
    uint64_t syscall_user_rsp;
    
    uint64_t load_start;
    uint64_t load_end;

    UserInfo *user_info; // 用户信息

    uint64_t fs_base; // FS段基址
    uint64_t fs;
    uint64_t clear_child_tid;
    page_directory_t *tid_directory;

    void   *sas_ss_sp;
    size_t  sas_ss_size;
    int     sas_ss_flags;

    vfs_node_t cwd;
    char      *str_cwd;

    char **argv; // 参数指针
    size_t argc; // 参数数量

    uint64_t winnum;
    bool     hasfscr;
    int      message_pipe_read_fd;
    int      message_pipe_write_fd;

    uint64_t window_count;

    uint64_t eevdf_vruntime;
    uint64_t eevdf_deadline;
    uint64_t eevdf_slice;
    uint64_t eevdf_last_start;

    bool owns_user_stack;
};

extern pcb_t kernel_group;
extern size_t   now_tid;

size_t   add_task(tcb_t task);
void     enable_scheduler();
void     disable_scheduler();
tcb_t    get_current_task();
uint64_t task_user_fs_selector(tcb_t task);
size_t   create_kernel_thread(void *_start, void *args, char *name, pcb_t pcb);
size_t   create_user_thread(void *_start, void *args, int argc, char *name, pcb_t pcb, char *cwd);
pcb_t    create_process_group(const char *name, pcb_t parent, page_directory_t *directory, char *cmdline);
uint64_t process_fork(struct X64_REGS *reg, bool is_vfork, uint64_t user_stack, uint64_t clone_flags = 0,
                      int *parent_tid = NULL, int *child_tid = NULL);
uint64_t thread_clone(struct X64_REGS *reg, uint64_t flags, uint64_t stack, int *parent_tid, int *child_tid,
                      uint64_t tls);
void     process_setup();
void     switch_to_user_mode();
void get_thread_name_from_filepath(char *path, char *name);

size_t create_message_thread(void *_start, char *name, pcb_t pcb, char *cwd, uint64_t arg);

void init_reaper();

typedef void (*elf_start)(int, char **, char **);
void kill_proc(pcb_t pcb, int exit_code, bool is_zombie);
bool kill_proc_deferred(pcb_t pcb, int exit_code);
void kill_thread(tcb_t task);
void kill_thread0(tcb_t task);
void kill_proc0(pcb_t pcb);
pcb_t found_pcb(int pid);
pcb_t found_process_by_exe_path(const char *exe_path);
tcb_t found_thread(pcb_t pcb, int tid);
