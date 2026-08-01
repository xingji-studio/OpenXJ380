#ifndef _BOOTLIB_H_
#define _BOOTLIB_H_
#include <efi.h>

// common.cpp

// 端口写（8位）
void outb(UINT16 port, UINT8 value);

// 端口读（8位）
UINT8 inb(UINT16 port);

// 端口写（16位）
void outw(UINT16 port, UINT16 value);

// 端口读（16位）
UINT16 inw(UINT16 port);

// 端口写（32位）
void outl(UINT16 port, UINT32 value);

// 端口读（32位）
UINT32 inl(UINT16 port);

// 从I/O端口批量地读取数据到内存（16位）
void insw(UINT16 port, VOID *buf, unsigned long n);

// 从内存批量地写入数据到I/O端口（16位）
void outsw(UINT16 port, const VOID *buf, unsigned long n);

// 从I/O端口批量地读取数据到内存（32位）
void insl(UINT32 port, VOID *addr, int cnt);

// 从内存批量地写入数据到I/O端口（32位）
void outsl(UINT32 port, const VOID *addr, int cnt);

// 复活中断
void enable_intr();

// 谋害中断
void disable_intr();

// MFENCE
void io_mfence();

// serial_port.cpp
#define PORT 0x3f8 // COM1

int init_serial();

int is_transmit_empty();

void write_serial(char a);

void write_serial_string(char *str);

// string.c
char *strcpy(char *dest, const char *src);

UINT64 strlen(const char *str);

UINT64 strcmp(char *from_str, char *cmp_str);

UINT64 part_strcmp(char *from_str, char *cmp_str, UINT64 size);

char *Hex2Char(unsigned long long hex);

char *Dec2Char(unsigned long long dec);
#endif // _BOOTLIB_H_