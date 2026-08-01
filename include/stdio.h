#ifndef _STDIO_H_
#define _STDIO_H_

#ifdef __cplusplus
extern "C" {
#endif

#define READ     0x2
#define WRITE    0x4
#define APPEND   0x8
#define BIN      0x0
#define PLUS     0x10
#define EOF      -1
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ   (4096 * 2)
#define getchar  getch

typedef struct FILE
{
    unsigned int   mode;
    unsigned int   fileSize;
    unsigned char *buffer;
    unsigned int   bufferSize;
    unsigned int   p;
    unsigned char  eof;
    unsigned char  read_flag; // 0 needn't to read, 1 need to read
    char          *name;
} FILE;

int          feof(FILE *stream);
int          ferror(FILE *stream);
int          fseek(FILE *fp, int offset, int whence);
long         ftell(FILE *stream);
FILE        *fopen(const char *filename, const char *mode);
int          fclose(FILE *fp);
int          fgetc(FILE *stream);
unsigned int fread(void *buffer, unsigned int size, unsigned int count, FILE *stream);
int          ungetc(int c, FILE *fp);

#ifdef __cplusplus
}
#endif

#endif
