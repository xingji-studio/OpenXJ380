#ifndef _XJ380_TYPE_H_
#define _XJ380_TYPE_H_

//将类型名称简化
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;



typedef void (*int_handler)();
typedef void (*irq_handler)(int irq);
typedef void (*task_f)();

typedef void *system_call;

#endif