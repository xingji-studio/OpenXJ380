#include "memory.h"

void swap(free_member *a, free_member *b) {
  free_member temp = *a;
  *a = *b;
  *b = temp;
}
int cmp(free_member a, free_member b) { return a.end <= b.end; }
int partition(free_member *arr, int low, int high) {
  free_member pivot = arr[high];
  int i = (low - 1);

  for (int j = low; j <= high - 1; j++) {
    if (cmp(arr[j], pivot)) {
      i++;
      swap(&arr[i], &arr[j]);
    }
  }

  swap(&arr[i + 1], &arr[high]);
  return (i + 1);
}

void quicksort(free_member *arr, int low, int high) {
  if (low < high) {
    int pi = partition(arr, low, high);
    quicksort(arr, low, pi - 1);
    quicksort(arr, pi + 1, high);
  }
}
freeinfo *make_next_freeinfo(memory *mem) {
  const int size = FREE_MAX_NUM * sizeof(free_member) + sizeof(freeinfo);
  freeinfo *fi = NULL;
  freeinfo *finf = mem->freeinf;
  freeinfo *old = NULL;
  uint32_t s, n;
  while (finf) {
    old = finf;
    for (int i = 0; i < FREE_MAX_NUM; i++) {
      if (finf->f[i].start + finf->f[i].end == 0) {
        break;
      }
      if (finf->f[i].end - finf->f[i].start >= size) {
        uint32_t start = finf->f[i].start;
        s = finf->f[i].start;
        n = finf->f[i].end;
        mem_delete(i, finf);
        fi = (freeinfo *)start;
        break;
      }
    }
    if (fi) {
      break;
    }
    finf = finf->next;
  }
  if (!fi) {
    mem->memerrno = ERRNO_NO_ENOGHT_MEMORY;
    return NULL;
  }
  fi->next = 0;
  while (finf) {
    old = finf;
    finf = finf->next;
  }
  old->next = fi;
  fi->f = (free_member *)((uint32_t)fi + sizeof(freeinfo));
  for (int i = 0; i < FREE_MAX_NUM; i++) {
    fi->f[i].start = 0;
    fi->f[i].end = 0;
  }

  if (n - s > size) {
    mem_free_finf(mem, fi, s + size, n - s - size); // 一点也不浪费
  }

  return fi;
}
free_member *mem_insert(int pos, freeinfo *finf) {
  int j = 0;
  for (int i = 0; i < FREE_MAX_NUM; i++) {
    if (finf->f[i].start + finf->f[i].end != 0) {
      ++j;
    }
  }
  if (j == FREE_MAX_NUM) {
    return NULL;
  }
  for (int i = j - 1; i >= pos; i--) {
    unsigned debug1 = (unsigned)(&(finf->f[i + 1]));
    unsigned debug2 = (unsigned)(&(finf->f[i]));
    if (!debug1 || !debug2) {
      for (;;)
        ;
    }
    finf->f[i + 1] = finf->f[i];
  }
  return &(finf->f[pos]);
}
free_member *mem_add(freeinfo *finf) {
  int j = -1;
  for (int i = 0; i < FREE_MAX_NUM; i++) {
    if (finf->f[i].start + finf->f[i].end == 0) {
      j = i;
      break;
    }
  }
  if (j == -1) {
    return NULL;
  }
  return &(finf->f[j]);
}
void mem_delete(int pos, freeinfo *finf) {
  int i;
  for (i = pos; i < FREE_MAX_NUM - 1; i++) {
    if (finf->f[i].start == 0 && finf->f[i].end == 0) {
      return;
    }
    finf->f[i] = finf->f[i + 1];
  }
  finf->f[i].start = 0;
  finf->f[i].end = 0;
}
uint32_t mem_get_all_finf(freeinfo *finf) {
  for (int i = 0; i < FREE_MAX_NUM; i++) {
    if (finf->f[i].start + finf->f[i].end == 0) {
      return i;
    }
  }
  return FREE_MAX_NUM;
}
// 内存整理
void mem_defragmenter(freeinfo *finf) {
  for (int i = 0; i < FREE_MAX_NUM - 1; i++) {
    if (finf->f[i].start + finf->f[i].end == 0) {
      break;
    }
    if (finf->f[i].end - finf->f[i].start == 0) {
      mem_delete(i, finf);
      continue;
    }
    if (finf->f[i].end == finf->f[i + 1].start) {
      int end = finf->f[i + 1].end;
      mem_delete(i + 1, finf);
      finf->f[i].end = end;
      continue;
    }
    if (finf->f[i + 1].start == finf->f[i].start) {
      int end = MEM_MAX(finf->f[i].end, finf->f[i + 1].end);
      mem_delete(i + 1, finf);
      finf->f[i].end = end;
      continue;
    }
    if (finf->f[i + 1].start < finf->f[i].end) {
      int end = MEM_MAX(finf->f[i].end, finf->f[i + 1].end);
      mem_delete(i + 1, finf);
      finf->f[i].end = end;
      continue;
    }
  }
}
int mem_free_finf(memory *mem, freeinfo *finf, void *p, uint32_t size) {
  quicksort(finf->f, 0, mem_get_all_finf(finf) - 1);
  mem_defragmenter(finf);
  free_member *tmp1 = NULL, // 第一（二）个连续的内存 其limit与start相等
      *tmp2 = NULL; // 第二（一）个连续的内存  其start与limit相等
  int idx1, idx2;
  // 遍历内存池，找到符合条件的两个格子（找不到也没关系）

  for (int i = 0; i < FREE_MAX_NUM; i++) {
    uintptr_t current_start = (uintptr_t)finf->f[i].start;
    uintptr_t current_end = (uintptr_t)finf->f[i].end;
    uintptr_t ptr_val = (uintptr_t)p;

    if (current_end == ptr_val) {
      tmp1 = &(finf->f[i]);
      idx1 = i;
    }
    if (current_start == ptr_val + size) {
      tmp2 = &(finf->f[i]);
      idx2 = i;
    }
  }

  if (!tmp1 && !tmp2) {             // 没有内存和他连续
                                    // for(;;);
    free_member *n = mem_add(finf); // 找一个空闲的格子放这块内存
    if (!n)
      return 0;
    // 配置这个格子
    n->start = p;
    n->end = (uint32_t)p + size;
    quicksort(finf->f, 0, mem_get_all_finf(finf) - 1);
    mem_defragmenter(finf);
    return 1;
  }
  // for(;;);
  //  两个都找到了，说明是个缺口
  if (tmp1 && tmp2) {
    tmp1->end = tmp2->end;
    mem_delete(idx2, finf);
    quicksort(finf->f, 0, mem_get_all_finf(finf) - 1);
    mem_defragmenter(finf);
    return 1;
  }
  if (tmp1) { // BUGFIX
    tmp1->end += size;
    quicksort(finf->f, 0, mem_get_all_finf(finf) - 1);
    mem_defragmenter(finf);
    return 1;
  }
  if (tmp2) {
    tmp2->start = p;
    quicksort(finf->f, 0, mem_get_all_finf(finf) - 1);
    mem_defragmenter(finf);
    return 1;
  }

  return 1;
}
void *mem_alloc_finf(memory *mem, freeinfo *finf, uint32_t size,
                     freeinfo *if_nomore) {
  free_member *choice = NULL;
  int choice_index = 0;
  for (int i = 0; i < FREE_MAX_NUM; i++) {
    if (finf->f[i].start == 0 && finf->f[i].end == 0) {
      break;
    }
    if (finf->f[i].end - finf->f[i].start >= size) {
      if (!choice) {
        choice = &(finf->f[i]);
        choice_index = i;
        continue;
      }
      if (finf->f[i].end - finf->f[i].start < choice->start - choice->end) {
        choice = &(finf->f[i]);
        choice_index = i;
        continue;
      }
    }
  }
  if (choice == NULL) {
    mem->memerrno = ERRNO_NO_ENOGHT_MEMORY;
    return NULL;
  }
  uint32_t start = choice->start;
  choice->start += size;
  if (choice->end - choice->start == 0) {
    mem_delete(choice_index, finf);
  }
  mem->memerrno = ERRNO_NOPE;
  mem_defragmenter(finf);
  memset(start, 0, size);

  return (void *)start;
}
void *mem_alloc(memory *mem, uint32_t size) {
  freeinfo *finf = mem->freeinf;
  int flag = 0;
  freeinfo *if_nomore = NULL;
  while (finf) {
    if (flag && !if_nomore) {
      break;
      ;
    }
    void *result = mem_alloc_finf(mem, finf, size, if_nomore);
    if (mem->memerrno != ERRNO_NOPE) {
      if (mem->memerrno == ERRNO_NO_MORE_FREE_MEMBER) {
        if (!flag) {
          if_nomore = finf;
          flag = 1;
        }
      }
    } else {
      return result;
    }
    if (flag) {
      if_nomore = if_nomore->next;
    } else {
      finf = finf->next;
    }
  }
  if (flag) {
    freeinfo *new_f = make_next_freeinfo(mem);
    if (!new_f) {
      return NULL;
    }
    return mem_alloc(mem, size);
  }
  return NULL;
}
void mem_free(memory *mem, void *p, uint32_t size) {
  freeinfo *finf = mem->freeinf;
  while (finf) {
    if (mem_free_finf(mem, finf, p, size)) {
      return;
    }
    finf = finf->next;
  }
  freeinfo *new_f = make_next_freeinfo(mem);
  if (new_f) {
    mem_free_finf(mem, new_f, p, size);
  }
}
void show_mem(memory *mem) {
}
memory *memory_init(uint32_t start, uint32_t size) {
  memory *mem;
  mem = (memory *)start;
  start += sizeof(memory);
  size -= sizeof(memory);
  if (size < 0) {
    for (;;)
      ;
  }
  mem->freeinf = (freeinfo *)start;
  start += sizeof(freeinfo);
  size -= sizeof(freeinfo);
  if (size < 0) {
    for (;;)
      ;
  }
  mem->freeinf->next = 0;
  mem->freeinf->f = (free_member *)start;
  start += FREE_MAX_NUM * sizeof(free_member);
  size -= FREE_MAX_NUM * sizeof(free_member);
  if ((int)size < 0) {
    for (;;)
      ;
  }
  for (int i = 0; i < FREE_MAX_NUM; i++) {
    mem->freeinf->f[i].start = 0;
    mem->freeinf->f[i].end = 0;
  }
  mem->memerrno = ERRNO_NOPE;
  mem_free(mem, (void *)start, size);
  return mem;
}
