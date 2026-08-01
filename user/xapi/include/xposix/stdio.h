#pragma once

#include "sys/types.h"
#include "../krlibc.h"

#ifdef __cplusplus
extern "C" {
#endif

ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);

#define READ     0x2
#define WRITE    0x4
#define APPEND   0x8
#define BIN      0x0
#define PLUS     0x10
#define EOF      -1
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define SEEK_DATA 3
#define SEEK_HOLE 4
#define BUFSIZ   (4096 * 2)
#define getchar  getch

typedef struct FILE
{
    int            fd;
    unsigned int   mode;
    unsigned int   fileSize;
    unsigned char *buffer;
    unsigned int   bufferSize;
    unsigned int   p;
    unsigned char  eof;
    unsigned char  read_flag; // 0 needn't to read, 1 need to read
    char          *name;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int          printf(const char *format, ...);
int          fprintf(FILE *stream, const char *format, ...);
int          dprintf(int fd, const char *format, ...);
int          sprintf(char *str, const char *format, ...);
int          snprintf(char *str, size_t size, const char *format, ...);
int          vprintf(const char *format, va_list ap);
int          vfprintf(FILE *stream, const char *format, va_list ap);
int          vdprintf(int fd, const char *format, va_list ap);
int          vsnprintf(char *str, size_t size, const char *format, va_list ap);

int          feof(FILE *stream);
int          ferror(FILE *stream);
int          fseek(FILE *fp, int offset, int whence);
long         ftell(FILE *stream);
FILE        *fopen(const char *filename, const char *mode);
int          fclose(FILE *fp);
int          fgetc(FILE *stream);
size_t       fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t       fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
char        *fgets(char *str, int size, FILE *stream);
int          fputc(int c, FILE *stream);
int          fputs(const char *str, FILE *stream);
int          fflush(FILE *stream);
int          fileno(FILE *stream);
int          ungetc(int c, FILE *fp);

#ifdef __cplusplus
}
#endif
