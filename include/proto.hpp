#ifndef _PROTO_H_
#define _PROTO_H_

#include "fs/vfs/list.h"
#ifndef CONFIG_KERNEL_TASK_STACK_SIZE
#define CONFIG_KERNEL_TASK_STACK_SIZE 1048576
#endif
#ifndef CONFIG_USER_STACK_SIZE
#define CONFIG_USER_STACK_SIZE 16777216
#endif
#ifndef CONFIG_USER_MMAP_START
#define CONFIG_USER_MMAP_START 0x0000400000000000UL
#endif

#define KERNEL_STACK_SIZE (CONFIG_KERNEL_TASK_STACK_SIZE * 1UL) // 任务内核栈地址
#define BIG_USER_STACK    (CONFIG_USER_STACK_SIZE * 1UL)        // 用户栈大小，要对齐到页

#include <ps2/keyboard.h>
#include <stdarg.h>
#include <stdint.h>

#define close_interrupt disable_intr()
#define open_interrupt  enable_intr()

/*
 *   graphics
 */

/*
 *
 *  driver/serial/
 * 
 */
int    snprintf(char *buf, size_t size, const char *fmt, ...);
// driver/serial/serial_port.cpp
int    init_serial();
void   write_serial(char a);
void   write_serial_string(const char *str);
void   write_serial_dec(unsigned long long dec);
void   write_serial_hex(unsigned long long hex);
int    write_serial_fmt(const char *fmt, ...);
void   write_serial_fmt_heapless(const char *fmt, ...);
void   serial_wprintf(const char *fmt, ...);
size_t vwprintf(Writer *writer, const char *fmt, va_list args);
int    sprintf(char *buf, const char *fmt, ...);

int printk(const char *fmt, ...);
int pr_debug(const char *fmt, ...);
int pr_warn(const char *fmt, ...);
int pr_info(const char *fmt, ...);
int pr_notice(const char *fmt, ...);
int pr_err(const char *fmt, ...);

/*
 *
 *  kernel/
 * 
 */

// kernel/wsod.cpp
#ifdef __cplusplus
extern "C" {
#endif
void wsod_unexpect_intrrupt(struct X64_REGS *regs, uint64_t error_code);
void c_device_not_avaliable(struct X64_REGS *regs, uint64_t error_code);
void wsod_divide_error(struct X64_REGS *regs, uint64_t error_code);
void wsod_undefined_opcode(struct X64_REGS *regs, uint64_t error_code);
void wsod_debug(struct X64_REGS *regs, uint64_t error_code);
void wsod_nmi(struct X64_REGS *regs, uint64_t error_code);
void wsod_double_fault(struct X64_REGS *regs, uint64_t error_code);
void wsod_system_kernel_error(struct X64_REGS *regs, uint64_t error_code);
void wsod_general_protection(struct X64_REGS *regs, uint64_t error_code);
void wsod_page_fault(struct X64_REGS *regs, uint64_t error_code);
void default_isr(struct X64_REGS *regs, uint64_t error_code);

#ifdef __cplusplus
}
#endif

// graphics/sheet.cpp

// graphics/window/theme.cpp


// graphics/window/windowm.cpp

// lib/utflib.c
typedef int32_t Rune;
#define UTFmax    6            /* maximum bytes per rune */
#define Runeerror ((Rune) - 1) /* decoding error in utf */
int    charntorune(Rune *, const char *, size_t);
int    chartorune(Rune *, const char *);
size_t utflen(const char *);
size_t utfnlen(const char *, size_t);

extern const unsigned char utftab[64];
/*
 *
 *  kernel/cpu/
 * 
 */

// kernel/cpu/common.cpp
void     outb(uint16_t port, uint8_t value);
uint8_t  inb(uint16_t port);
void     outw(uint16_t port, uint16_t value);
uint16_t inw(uint16_t port);
void     outl(uint16_t port, uint32_t value);
uint32_t inl(uint16_t port);
void     insw(uint16_t port, void *buf, unsigned long n);
void     outsw(uint16_t port, const void *buf, unsigned long n);
void     insl(uint32_t port, void *addr, int cnt);
void     outsl(uint32_t port, const void *addr, int cnt);
void     enable_intr();
void     disable_intr();
void     io_mfence();

// kernel/cpu/cpuid.cpp
void asm_cpuid(uint32_t mop, uint32_t sop, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
void get_cpu_name(char *c);
void get_cpu_vendor(char *c);
bool mtrr_supported();

// kernel/cpu/mtrr.cpp
void mtrr_save();
void mtrr_restore();

// kernel/cpu/delay.cpp
// 读取TSC
static inline uint64_t rdtsc();
void                   delay(uint32_t count);
uint64_t               get_cpu_freq_mhz();
void                   delay_us(uint64_t us);
void                   delay_ms(uint64_t ms);
void                   delay_s(uint64_t s);
void                   delay_us_hp(uint64_t us);
void                   delay_ms_hp(uint64_t ms);
void                   delay_s_hp(uint64_t s);

/*
 *
 *  kernel/intr/
 * 
 */

// kernel/intr/8259a.cpp
void init_8259A();
void disable_8259A();

// kernel/intr/apic.cpp
void     lapic_write(uint32_t reg, uint64_t value);
uint32_t lapic_read(uint32_t reg);
void     ioapic_mask_all();
void     init_lApic();
void     init_apic(uint64_t MADT0);
uint64_t lapic_id();

// kernel/intr/hpet.cpp
uint64_t nanoTime();
uint64_t bootNanoTime();
void     init_hpet(uint64_t hpet_ptr);

/*
 *
 *  kernel/pctable/
 * 
 */

// kernel/pctable/idt.cpp
void init_idt();

// kernel/pctable/gdt.cpp
void init_gdt();

/*
 *
 *  kernel/memory/
 * 
 */

// kernel/memory/heap/heap.cpp
#include <mm/alloc/alloc.h>
void init_heap();

#include <mm/memory.h>

// kernel/memory/bitmap.cpp
#include <mm/bitmap.h>

// kernel/memory/frame.cpp
void init_frame(MEMORY_MAP map);
void free_frames(uint64_t addr, size_t size);
void free_frame(uint64_t addr)
{
    free_frames(addr, 1);
}
uint64_t alloc_frames(size_t count);
uint64_t alloc_frames_dma32(size_t count);

// kernel/memory/hhdm.cpp
void     init_hhdm();
uint64_t get_physical_memory_offset();
void    *phys_to_virt(uint64_t phys_addr);
void    *virt_to_phys(uint64_t virt_addr);
void    *driver_phys_to_virt(uint64_t phys_addr);
void    *driver_virt_to_phys(uint64_t virt_addr);
uint64_t page_virt_to_phys(uint64_t va);
#define USER_MMAP_START CONFIG_USER_MMAP_START // 用户堆映射起始地址
// kernel/memory/page.cpp
#include <mm/page.h>

/*
 *
 *  kernel/smp/
 * 
 */

#include <smp/smp.h>

// kernel/smp/smp.cpp
void init_smp(uint64_t MADT0);
void set_kernel_stack(uint64_t rsp);

extern "C" void        send_eoi();
struct PROCESSOR_INFO *get_current_cpu();

extern const FrameBufferConfig *fbc_addr;

/*
 *   font
 */

// driver/ps2/keyboard.cpp
// extern struct keyboard_buf kb_fifo;
// extern uint8_t             keyboard_code[256];
// extern uint8_t             keyboard_code1[256];
#ifdef __cplusplus
extern "C" {
#endif
void keyboard_push_input(uint8_t value);
void keyboard_usb_key_event(uint8_t usage, uint8_t value, uint8_t pressed);
#ifdef __cplusplus
}
#endif

// driver/ps2/mouse.cpp
void mouse_init();
void process_mouse_info();

// uint8_t get_Mouse_output();
bool mousedecode(uint8_t data);
int  get_mouse_x();
int  get_mouse_y();
int  get_mouse_scroll();
void set_mouse_position(int x, int y);
void center_mouse_cursor();
void draw_mouse(SHEET_INFO *sht, SHEET *csheet);
void move_mouse(SHEET_INFO *sht, SHEET *csheet, int px, int py);
#ifdef __cplusplus
extern "C" {
#endif
void mouse_inject_report(int dx, int dy, uint8_t buttons, int wheel);
#ifdef __cplusplus
}
#endif

/*
 *
 *  kernel/task/
 * 
 */

// kernel/task/user.cpp
void create_user_thread_from_file(char *path, pcb_t pcb);
int create_user_process_from_file(char *path, pcb_t pcb, char *argv[]);
int create_user_process_singleton_from_file(char *path, pcb_t pcb, char *argv[]);
void PrintPicture(SHEET_INFO *sht, SHEET *csheet, int x, int y, int ow, int oh, char *path);
void PrintPicture_blend(SHEET_INFO *sht, SHEET *csheet, int x, int y, int ow, int oh, char *path);
bool LoadPicture(SHEET_BUFFER *buffer, int ow, int oh, char *path);
void LoadPictureOgM(SHEET_BUFFER **buffer, char *path);
void GetPictureSize(int *w, int *h, char *path);
void copy_buffer_by_id(SHEET_INFO *sht, SHEET *dst_sheet, SHEET_BUFFER *src, uint32_t dst_x, uint32_t dst_y,
                       uint32_t width, uint32_t height);
void copy_buffer_by_id_without_alpha(SHEET_INFO *sht, SHEET *dst_sheet, SHEET_BUFFER *src, uint32_t dst_x,
                                     uint32_t dst_y, uint32_t width, uint32_t height);
void copy_buffer_blend_by_id(SHEET_INFO *sht, SHEET *dst_sheet, SHEET_BUFFER *src, uint32_t dst_x, uint32_t dst_y,
                             uint32_t width, uint32_t height);

void backtrace(struct X64_REGS *regs);

uint64_t get_counter();
uint32_t get_freq();

char     getch_from_file(void *buffer);
uint64_t getline_from_file(void *buffer, char *str);

// ulog.cpp
void init_xuls();
void write_ulog(char *str);
void ulog_err(char *err_type);

#endif
