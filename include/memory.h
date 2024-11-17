#ifndef __MEMORY__
#define __MEMORY__
typedef unsigned int uintptr_t;
typedef unsigned int uint32_t;
//shit山，无任何优化的内存管理头文件
#define NULL 0
#define FREE_MAX_NUM 4096
#define ERRNO_NOPE 0
#define ERRNO_NO_ENOGHT_MEMORY 1
#define ERRNO_NO_MORE_FREE_MEMBER 2
#define MEM_MAX(a, b) (a) > (b) ? (a) : (b)
typedef struct {
  uint32_t start;
  uint32_t end; // end和start都等于0说明这个free结构没有使用
} free_member;
typedef struct freeinfo freeinfo;
typedef struct freeinfo {
  free_member *f;
  freeinfo *next;
} freeinfo;
typedef struct {
  freeinfo *freeinf;
  int memerrno;
} memory;
memory *memory_init(uint32_t start, uint32_t size);
void *mem_alloc(memory *mem, uint32_t size);
void mem_free(memory *mem, void *p, uint32_t size);
#endif