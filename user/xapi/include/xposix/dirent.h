#pragma once

#include "sys/types.h"

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
    long           d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct DIR {
    int fd;
    int pos;
    int len;
    char buf[4096];
} DIR;

#ifdef __cplusplus
extern "C" {
#endif

int getdents(int fd, struct dirent *dents, size_t size);
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif
