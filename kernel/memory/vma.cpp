#include "mm/vma.h"
#include "proto.hpp"
#include "krlibc.h"

vma_t *vma_alloc(void) {
    vma_t *vma = (vma_t *)malloc(sizeof(vma_t));
    if (!vma) return NULL;

    memset(vma, 0, sizeof(vma_t));
    vma->vm_fd  = -1;
    vma->shm_id = -1;
    return vma;
}

void vma_free(vma_t *vma) {
    if (vma) {
        if (vma->vm_name) free(vma->vm_name);
        free(vma);
    }
}

vma_t *vma_find(vma_manager_t *mgr, unsigned long addr) {
    vma_t *vma = mgr->vma_list;

    while (vma) {
        if (addr >= vma->vm_start && addr < vma->vm_end) { return vma; }
        vma = vma->vm_next;
    }
    return NULL;
}

vma_t *vma_find_intersection(vma_manager_t *mgr, unsigned long start, unsigned long end) {
    vma_t *vma = mgr->vma_list;

    while (vma) {
        if (!(end <= vma->vm_start || start >= vma->vm_end)) { return vma; }
        vma = vma->vm_next;
    }
    return NULL;
}

// 插入VMA到链表（保持地址排序）
int vma_insert(vma_manager_t *mgr, vma_t *new_vma) {
    if (!new_vma) return -1;

    // 检查是否有重叠
    if (vma_find_intersection(mgr, new_vma->vm_start, new_vma->vm_end)) { return -1; }

    vma_t *vma  = mgr->vma_list;
    vma_t *prev = NULL;

    // 找到正确的插入位置
    while (vma && vma->vm_start < new_vma->vm_start) {
        prev = vma;
        vma  = vma->vm_next;
    }

    // 插入VMA
    new_vma->vm_next = vma;
    new_vma->vm_prev = prev;

    if (prev) {
        prev->vm_next = new_vma;
    } else {
        mgr->vma_list = new_vma;
    }

    if (vma) { vma->vm_prev = new_vma; }

    mgr->vm_used += new_vma->vm_end - new_vma->vm_start;
    return 0;
}

// 从链表中移除VMA
int vma_remove(vma_manager_t *mgr, vma_t *vma) {
    if (!vma) return -1;

    if (vma->vm_prev) {
        vma->vm_prev->vm_next = vma->vm_next;
    } else {
        mgr->vma_list = vma->vm_next;
    }

    if (vma->vm_next) { vma->vm_next->vm_prev = vma->vm_prev; }

    mgr->vm_used -= vma->vm_end - vma->vm_start;
    return 0;
}

// VMA分割
int vma_split(vma_t *vma, unsigned long addr) {
    if (!vma || addr <= vma->vm_start || addr >= vma->vm_end) { return -1; }

    // 创建新的VMA
    vma_t *new_vma = vma_alloc();
    if (!new_vma) return -1;

    // 复制属性
    *new_vma          = *vma;
    new_vma->vm_start = addr;
    new_vma->vm_next  = vma->vm_next;
    new_vma->vm_prev  = vma;
    new_vma->vm_name  = vma->vm_name ? strdup(vma->vm_name) : NULL;

    // 调整文件偏移量
    if (vma->vm_type == VMA_TYPE_FILE) { new_vma->vm_offset += addr - vma->vm_start; }

    // 更新原VMA
    vma->vm_end  = addr;
    vma->vm_next = new_vma;

    // 更新链表
    if (new_vma->vm_next) { new_vma->vm_next->vm_prev = new_vma; }

    return 0;
}

// VMA合并
int vma_merge(vma_t *vma1, vma_t *vma2) {
    if (!vma1 || !vma2 || vma1->vm_end != vma2->vm_start) { return -1; }

    // 检查是否可以合并（相同属性）
    if (vma1->vm_flags != vma2->vm_flags || vma1->vm_type != vma2->vm_type ||
        vma1->vm_fd != vma2->vm_fd) {
        return -1;
    }

    // 合并VMA
    vma1->vm_end  = vma2->vm_end;
    vma1->vm_next = vma2->vm_next;

    if (vma2->vm_next) { vma2->vm_next->vm_prev = vma1; }

    vma_free(vma2);
    return 0;
}

int vma_unmap_range(vma_manager_t *mgr, uintptr_t start, uintptr_t end) {
    if (!mgr || start >= end) return -1;

    vma_t *vma = mgr->vma_list;

    while (vma) {
        if (vma->vm_end <= start) {
            vma = vma->vm_next;
            continue;
        }
        if (vma->vm_start >= end) break;

        // 完全包含在要取消映射的范围内
        if (vma->vm_start >= start && vma->vm_end <= end) {
            vma_t *next = vma->vm_next;
            vma_remove(mgr, vma);
            vma_free(vma);
            vma = next;
            continue;
        }

        if (vma->vm_start < start && vma->vm_end > end) {
            // VMA crosses the unmapped hole. Keep the left part in place and
            // split off the right part so vm_used drops by exactly the hole.
            if (vma_split(vma, end) != 0) return -1;
            vma->vm_end = start;
            mgr->vm_used -= end - start;
            vma = vma->vm_next;
            continue;
        }

        if (vma->vm_start < start) {
            uintptr_t old_end = vma->vm_end;
            vma->vm_end       = start;
            mgr->vm_used -= old_end - start;
            vma = vma->vm_next;
            continue;
        }

        if (vma->vm_end > end) {
            uintptr_t old_start = vma->vm_start;
            vma->vm_start       = end;
            if (vma->vm_type == VMA_TYPE_FILE) { vma->vm_offset += end - old_start; }
            mgr->vm_used -= end - old_start;
            break;
        }

        vma = vma->vm_next;
    }

    return 0;
}

void vma_manager_exit_cleanup(vma_manager_t *mgr) {
    if (!mgr) return;

    vma_t *vma = mgr->vma_list;
    vma_t *next;
    int    cleaned_count = 0;

    // 遍历并清理所有VMA
    while (vma) {
        next = vma->vm_next;

        // 从链表中移除
        if (vma->vm_prev) {
            vma->vm_prev->vm_next = vma->vm_next;
        } else {
            mgr->vma_list = vma->vm_next;
        }

        if (vma->vm_next) { vma->vm_next->vm_prev = vma->vm_prev; }

        // 更新统计信息
        mgr->vm_used -= vma->vm_end - vma->vm_start;

        // 释放VMA结构体
        vma_free(vma);
        cleaned_count++;

        vma = next;
    }

    // 重置管理器状态
    mgr->vma_list = NULL;
    mgr->vm_total = 0;
    mgr->vm_used  = 0;
}

static bool vma_clone_name(vma_t *dst, const vma_t *src)
{
    if (src->vm_name == NULL)
    {
        dst->vm_name = NULL;
        return true;
    }

    size_t name_len = strlen(src->vm_name) + 1;
    dst->vm_name = (char *)malloc(name_len);
    if (dst->vm_name == NULL) return false;

    memcpy(dst->vm_name, src->vm_name, name_len);
    return true;
}

bool vma_manager_clone(vma_manager_t *src_mgr,vma_manager_t *dst_mgr) {
    if (!src_mgr) {
        return false;
    }
    if (!dst_mgr) {
        return false;
    }

    // 2. 复制管理器的基本信息
    dst_mgr->vma_list = NULL; // 链表头先设为 NULL
    dst_mgr->vm_total = src_mgr->vm_total;
    dst_mgr->vm_used  = 0; // vm_used 将在插入 VMA 时更新

    vma_t *src_vma = src_mgr->vma_list;
    vma_t *new_vma = NULL;

    // 3. 遍历源 VMA 链表并深拷贝
    while (src_vma) {
        if (src_vma->vm_start >= src_vma->vm_end) {
            write_serial_fmt("[vma-clone] invalid range start=0x%llx end=0x%llx flags=0x%llx type=%d\n",
                             src_vma->vm_start,
                             src_vma->vm_end,
                             src_vma->vm_flags,
                             src_vma->vm_type);
            vma_manager_exit_cleanup(dst_mgr);
            return false;
        }
        vma_t *src_overlap = src_mgr->vma_list;
        while (src_overlap != NULL)
        {
            if (src_overlap != src_vma &&
                !(src_vma->vm_end <= src_overlap->vm_start || src_vma->vm_start >= src_overlap->vm_end))
            {
                write_serial_fmt("[vma-clone] source overlap a=0x%llx-0x%llx b=0x%llx-0x%llx\n",
                                 src_vma->vm_start,
                                 src_vma->vm_end,
                                 src_overlap->vm_start,
                                 src_overlap->vm_end);
                vma_manager_exit_cleanup(dst_mgr);
                return false;
            }
            src_overlap = src_overlap->vm_next;
        }

        // 3.1. 分配新的 VMA 节点
        new_vma = vma_alloc();
        if (!new_vma) {
            write_serial_fmt("[vma-clone] alloc node failed start=0x%llx end=0x%llx\n",
                             src_vma->vm_start,
                             src_vma->vm_end);
            // 如果分配失败，必须清理已复制的部分
            // vma_manager_exit_cleanup 适合清理，但需要保证它能正确处理部分构建的链表
            // 这里我们使用一个更轻量级的清理，因为 vma_insert 并没有完全依赖 mgr->vm_used
            // 但是 vma_manager_exit_cleanup 更健壮
            vma_manager_exit_cleanup(dst_mgr);
            return false;
        }

        // 3.2. 复制 VMA 的基本属性
        // 复制除了指针以外的所有字段
        new_vma->vm_start  = src_vma->vm_start;
        new_vma->vm_end    = src_vma->vm_end;
        new_vma->vm_flags  = src_vma->vm_flags;
        new_vma->vm_type   = src_vma->vm_type;
        new_vma->vm_fd     = src_vma->vm_fd;
        new_vma->vm_offset = src_vma->vm_offset;
        new_vma->shm_id    = src_vma->shm_id;

        // 链表指针在 vma_alloc 中初始化为 NULL，在 vma_insert 中设置

        // 3.3. 深拷贝 vm_name
        if (!vma_clone_name(new_vma, src_vma)) {
            write_serial_fmt("[vma-clone] alloc name failed start=0x%llx end=0x%llx name=0x%llx\n",
                             src_vma->vm_start,
                             src_vma->vm_end,
                             (uint64_t)src_vma->vm_name);
            // 如果名称分配失败，清理并退出
            vma_free(new_vma);
            vma_manager_exit_cleanup(dst_mgr);
            return false;
        }

        if (vma_insert(dst_mgr, new_vma) != 0) {
            write_serial_fmt("[vma-clone] insert failed start=0x%llx end=0x%llx overlap=0x%llx\n",
                             new_vma->vm_start,
                             new_vma->vm_end,
                             (uint64_t)vma_find_intersection(dst_mgr, new_vma->vm_start, new_vma->vm_end));
            // 这不应该发生，除非 vm_start/vm_end 出错
            vma_free(new_vma);
            vma_manager_exit_cleanup(dst_mgr);
            return false;
        }

        src_vma = src_vma->vm_next;
    }
    return true;
}
