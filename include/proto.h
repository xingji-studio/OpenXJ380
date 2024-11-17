#ifndef _XJ380_PROTO_H_
#define _XJ380_PROTO_H_
//声明，作用相当于30day里的bootpack.h
// drivers/keyboard.c
PUBLIC void init_keyboard();
PUBLIC void kb_wait();

// drivers/mouse.c
PUBLIC void enable_mouse();
PUBLIC int mouse_decode(MOUSE_DEC *mdec, u8 data);
PUBLIC void init_mouse();
PUBLIC void mouse_sheet_update_to(u16 height);

// graphics/rect.c
PUBLIC void draw_rect(int x0, int y0, int x1, int y1, int c);
PUBLIC void boxfill8(u32 *buf, int xsize, int c, int x0, int y0, int x1, int y1);

// graphics/text.c
PUBLIC void putfont8(u32 *buf, int xsize, int x, int y, int c, char *font);
PUBLIC void put_char(char c);
PUBLIC void put_str(char *s);
PUBLIC void putstr_atbuf(u32 *buf, int xsize, int x, int y, int c, char *s);
PUBLIC void setX(int x);
PUBLIC void setY(int y);
PUBLIC int getX();
PUBLIC int getY();

// graphics/mouse_pointer.c
PUBLIC void init_mouse_cursor(int *buf, int bc);

// gui/sheet.c
PUBLIC void shtctl_init(SHTCTL *ctl);
PUBLIC SHEET *sheet_alloc(SHTCTL *ctl);
PUBLIC void sheet_setbuf(SHEET *sheet, u32 *buf, int xsize, int ysize, int col_inv);
PUBLIC void sheet_updown(SHEET *sht, int height);
PUBLIC void sheet_refreshsub(SHTCTL *ctl, int vx0, int vy0, int vx1, int vy1);
PUBLIC void sheet_refresh(SHEET *sht, int bx0, int by0, int bx1, int by1);
PUBLIC void sheet_slide(SHEET *sht, int vx0, int vy0);
PUBLIC void sheet_free(SHEET *sht);

// gui/vram.c
PUBLIC void DrawStar(int x, int y, int size, int color);
PUBLIC void init_screen();
PUBLIC void DrawStartMenu(int color);
PUBLIC void computer();
PUBLIC void reDrawComputer();
PUBLIC void DrawCommand();
PUBLIC void reDrawCommand();
PUBLIC void BetaWaterMark();

// gui/window.c
PUBLIC void make_window(u32 *buf, int xsize, int ysize, char *title, int bc);
PUBLIC void make_console_window(u32 *buf, int xsize, int ysize, char *title, int bc);

// kernel/clock.c
PUBLIC void clock_handler(int irq);
PUBLIC void milli_delay(int ms);
PUBLIC void Sleep(int ms);
PUBLIC void init_clock();

// kernel/fifo.c
PUBLIC void fifo_init(FIFO *fifo, int size, u32 *buf);
PUBLIC int fifo_put(FIFO *fifo, u32 data);
PUBLIC int fifo_get(FIFO *fifo);
PUBLIC int fifo_status(FIFO *fifo);

// kernel/gdt.c
PUBLIC void init_gdt();

// kernel/i8259.c
PUBLIC void init_8259A();
PUBLIC void put_irq_handler(int irq, irq_handler handler);
PUBLIC void spurious_irq(int irq);

// kernel/intr.c
PUBLIC void exception_handler(int vec_no, int err_code, int eip, int cs, int eflags);
PUBLIC void init_idt();

// kernel/kernel.asm
PUBLIC void restart();
PUBLIC void sys_call();

// kernel/main.c
PUBLIC void TestA();
PUBLIC void TestB();
PUBLIC void TestC();
PUBLIC void UpExplorer();
PUBLIC void UpCommand();

// kernel/proc.c
PUBLIC void schedule();

// kernel/start.c
PUBLIC void cstart();

// lib/klib.c
PUBLIC void put_int(int i);
PUBLIC void delay(int time);

// lib/kliba.asm
PUBLIC void out_byte(u16 port, u8 value);
PUBLIC u8   in_byte(u16 port);
PUBLIC void disable_irq(int irq);
PUBLIC void enable_irq(int irq);
PUBLIC void disable_int();
PUBLIC void enable_int();

// lib/printf.c
PUBLIC u32 printf(const char *fmt, ...);
PUBLIC u32 sprintf(char *buf, const char *fmt, ...);
PUBLIC u32 vsprintf(char *buf, const char *fmt, va_list args);

//	kernel/console.c
PUBLIC void start_console(SHEET *sht_cons);
PUBLIC void console_scroll();
PUBLIC void check_command(char *str);
PUBLIC void command_dir();
PUBLIC void command_cls();
PUBLIC void command_info();
PUBLIC void printstr(char *str);
PUBLIC void endline();
PUBLIC void yuanjiao(u32 *buf);

//	/system_api.c
PUBLIC void system_api(int edi, int esi, int ebp, int esp, int ebx, int edx, int ecx, int eax);

// system calls here
// kernel/syscall_impl.c
PUBLIC int sys_get_ticks();
PUBLIC int sys_write(char *buf);

// kernel/syscall.asm
PUBLIC int get_ticks();
PUBLIC int write(char *buf);

#endif