#ifndef _XJ380_CONST_H_
#define _XJ380_CONST_H_

#define PUBLIC
#define PRIVATE static

#define EXTERN  extern

#define GDT_SIZE 128
#define IDT_SIZE 256

#define NULL     ((void *) 0)

#define NR_IRQ       16
#define CLOCK_IRQ    0
#define KEYBOARD_IRQ 1
#define CASCADE_IRQ  2
#define MOUSE_IRQ    12
#define AT_WINI_IRQ  14

#define NR_SYS_CALL  2

#define TIMER0         0x40
#define TIMER_MODE     0x43
#define RATE_GENERATOR 0x34

#define TIMER_FREQ     1193182
#define HZ             100

#define false 0
#define true  1

#define KB_DATA  0x60
#define KB_CMD   0x64
#define KB_ACK   0xfa
#define LED_CODE 0xed

#endif