#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <errno.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/sys.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <mm/uaccess.h>
#include <pci/pci.h>
#include <pctable/gdt.h>
#include <pctable/idt.h>
#include <proto.hpp>
#include <rtc.h>
#include <syscall/pxapi.h>
#include <syscall/syscall.h>
#include <syscall/xapi_user.h>
#include <task/pcb.h>

static ssize_t xfile_write_from_user(vfs_node_t node, const void *buffer, size_t offset, size_t size)
{
    if (node == NULL) return -EIO;
    if (size == 0) return 0;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL || buffer == NULL) return -EFAULT;

    size_t bounce_bytes = MIN(size, XAPI_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_bytes);
    if (bounce == NULL) return -ENOMEM;

    size_t total = 0;
    while (total < size)
    {
        size_t chunk = MIN(size - total, bounce_bytes);
        size_t io_offset = 0;
        if (!xapi_checked_add_size(offset, total, &io_offset))
        {
            free(bounce);
            return total > 0 ? (ssize_t)total : -EINVAL;
        }
        if (!copy_from_user_pagedir(pagedir, bounce, (const uint8_t *)buffer + total, chunk))
        {
            free(bounce);
            return total > 0 ? (ssize_t)total : -EFAULT;
        }

        size_t wrote = vfs_write(node, bounce, io_offset, chunk);
        if (wrote == (size_t)VFS_STATUS_FAILED)
        {
            free(bounce);
            return total > 0 ? (ssize_t)total : -EIO;
        }

        total += wrote;
        if (wrote < chunk) break;
    }

    free(bounce);
    return (ssize_t)total;
}

static ssize_t xfile_read_to_user(vfs_node_t node, void *buffer, size_t offset, size_t size)
{
    if (node == NULL) return -EIO;
    if (size == 0) return 0;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL || buffer == NULL) return -EFAULT;

    size_t bounce_bytes = MIN(size, XAPI_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_bytes);
    if (bounce == NULL) return -ENOMEM;

    size_t total = 0;
    while (total < size)
    {
        size_t chunk = MIN(size - total, bounce_bytes);
        size_t io_offset = 0;
        if (!xapi_checked_add_size(offset, total, &io_offset))
        {
            free(bounce);
            return total > 0 ? (ssize_t)total : -EINVAL;
        }

        size_t got = vfs_read(node, bounce, io_offset, chunk);
        if (got == (size_t)VFS_STATUS_FAILED)
        {
            free(bounce);
            return total > 0 ? (ssize_t)total : -EIO;
        }
        if (got == 0) break;

        if (!copy_to_user_pagedir(pagedir, (uint8_t *)buffer + total, bounce, got))
        {
            free(bounce);
            return total > 0 ? (ssize_t)total : -EFAULT;
        }

        total += got;
        if (got < chunk) break;
    }

    free(bounce);
    return (ssize_t)total;
}

static char *xfile_build_user_path(uint64_t user_path)
{
    char *path = NULL;
    if (xapi_copy_string_from_user(&path, (const char *)user_path, XAPI_USER_PATH_MAX) < 0) return NULL;

    char *fpath = vfs_cwd_path_build(path);
    free(path);
    return fpath;
}


uint64_t do_xapi_OpenFile(uint64_t path)
{
    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL || path == 0) return 0;

    char *fpath = xfile_build_user_path(path);
    if (fpath == NULL) return 0;

    uint64_t fsptr_addr = page_alloc_random(pagedir, sizeof(XFILE), PTE_PRESENT | PTE_WRITEABLE | PTE_USER);
    if ((int64_t)fsptr_addr == -1)
    {
        free(fpath);
        return 0;
    }

    XFILE local;
    memset(&local, 0, sizeof(local));
    strncpy(local.path, fpath, sizeof(local.path) - 1);

    vfs_node_t f = vfs_open(fpath);
    if (!f)
    {
        free(fpath);
        unmap_page_range(pagedir, fsptr_addr, sizeof(XFILE));
        return 0;
    }

    uint64_t length = f->size;
    local.length    = length;
    if (length > 0)
    {
        if (length > (uint64_t)((size_t)-1))
        {
            vfs_close(f);
            free(fpath);
            unmap_page_range(pagedir, fsptr_addr, sizeof(XFILE));
            return 0;
        }

        local.buffer = (void *)page_alloc_random(pagedir, (size_t)length, PTE_PRESENT | PTE_WRITEABLE | PTE_USER);
        if ((int64_t)local.buffer == -1)
        {
            vfs_close(f);
            free(fpath);
            unmap_page_range(pagedir, fsptr_addr, sizeof(XFILE));
            return 0;
        }
        if (xfile_read_to_user(f, local.buffer, 0, (size_t)length) < 0)
        {
            vfs_close(f);
            free(fpath);
            unmap_page_range(pagedir, (uint64_t)local.buffer, (size_t)length);
            unmap_page_range(pagedir, fsptr_addr, sizeof(XFILE));
            return 0;
        }
    }
    vfs_close(f);
    free(fpath);

    if (!copy_to_user_pagedir(pagedir, (void *)fsptr_addr, &local, sizeof(local)))
    {
        if (local.length > 0 && local.buffer != NULL)
            unmap_page_range(pagedir, (uint64_t)local.buffer, (size_t)local.length);
        unmap_page_range(pagedir, fsptr_addr, sizeof(XFILE));
        return 0;
    }

    return fsptr_addr;
}

void do_xapi_CloseFile(uint64_t ptr)
{
    if (ptr == 0) return;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL) return;
    XFILE *fsptr = (XFILE *)ptr;
    XFILE local;
    memset(&local, 0, sizeof(local));
    if (!copy_from_user_pagedir(pagedir, &local, fsptr, sizeof(local)))
    {
        return;
    }
    local.path[sizeof(local.path) - 1] = '\0';

    char *fpath = vfs_cwd_path_build(local.path);
    if (fpath == NULL)
    {
        return;
    }

    vfs_node_t f = vfs_open(fpath);
    if (!f)
    {
        free(fpath);
        goto unmap_only;
    }

    if (local.length > 0)
    {
        ssize_t ret = xfile_write_from_user(f, local.buffer, 0, local.length);
        if (ret >= 0) vfs_update(f);
    }
    free(fpath);
    vfs_close(f);

unmap_only:
    if (local.length > 0 && local.buffer != NULL && user_range_mapped(pagedir, local.buffer, local.length))
    {
        unmap_page_range(pagedir, (uint64_t)local.buffer, local.length);
    }
    unmap_page_range(pagedir, (uint64_t)fsptr, sizeof(XFILE));
}

void do_xapi_SearchFile(uint64_t path, uint64_t count, uint64_t dir)
{
    page_directory_t *pagedir = xapi_current_pagedir();
    uint32_t          fcount  = 0;
    XAPIT_DirNode    *fdir    = (XAPIT_DirNode *)dir;
    if (pagedir == NULL || count == 0 || fdir == NULL || path == 0) return;

    copy_to_user_pagedir(pagedir, (void *)count, &fcount, sizeof(fcount));

    char *fpath = xfile_build_user_path(path);
    if (fpath == NULL)
    {
        fcount = 404;
        copy_to_user_pagedir(pagedir, (void *)count, &fcount, sizeof(fcount));
        return;
    }

    vfs_node_t v = vfs_open(fpath);
    if (!v)
    {
        fcount = 404;
        copy_to_user_pagedir(pagedir, (void *)count, &fcount, sizeof(fcount));
        free(fpath);
        return;
    }
    if (!(v->type & file_dir))
    {
        fcount = 404;
        copy_to_user_pagedir(pagedir, (void *)count, &fcount, sizeof(fcount));
        vfs_close(v);
        free(fpath);
        return;
    }
    vfs_child_lock();
    for (list_t node = v->child; node != NULL; node = node->next)
    {
        if (fcount >= 255)
        {
            fcount++;
            copy_to_user_pagedir(pagedir, (void *)count, &fcount, sizeof(fcount));
            vfs_child_unlock();
            vfs_close(v);
            free(fpath);
            return;
        }
        void *data = node->data;
        if (strcmp(((vfs_node_t)data)->name, "") != 0 && ((vfs_node_t)data)->type != file_delete)
        {
            XAPIT_DirNode entry;
            memset(&entry, 0, sizeof(entry));
            strncpy(entry.filename, ((vfs_node_t)data)->name, sizeof(entry.filename) - 1);
            entry.length   = ((vfs_node_t)data)->size;
            entry.filetype = ((vfs_node_t)data)->type == file_dir ? 1 : 0;
            if (!copy_to_user_pagedir(pagedir, &fdir[fcount], &entry, sizeof(entry)))
            {
                vfs_child_unlock();
                vfs_close(v);
                free(fpath);
                return;
            }
            fcount++;
        }
    }
    vfs_child_unlock();
    vfs_close(v);
    free(fpath);
    copy_to_user_pagedir(pagedir, (void *)count, &fcount, sizeof(fcount));
}

uint64_t do_xapi_ReadFile(struct X64_REGS *regs)
{
    char *fpath = xfile_build_user_path(regs->rdi);
    if (fpath == NULL)
    {
        regs->rax = (uint64_t)-EFAULT;
        return 0;
    }
    vfs_node_t node = vfs_open(fpath);
    if (!node)
    {
        regs->rax = (uint64_t)-ENOENT;
        free(fpath);
        return 0;
    }

    if (node->type & file_dir)
    {
        vfs_close(node);
        free(fpath);
        regs->rax = (uint64_t)-EISDIR;
        return 0;
    }

    ssize_t ret = xfile_read_to_user(node, (void *)regs->rsi, regs->r9, regs->rdx);
    vfs_close(node);
    free(fpath);
    return ret;
}

uint64_t do_xapi_WriteFile(struct X64_REGS *regs)
{
    char *fpath = xfile_build_user_path(regs->rdi);
    if (fpath == NULL)
    {
        regs->rax = (uint64_t)-EFAULT;
        return 0;
    }
    vfs_node_t node = vfs_open(fpath);
    if (!node)
    {
        // 文件不存在，创建它
        int create_ret = vfs_mkfile(fpath);
        if (create_ret < 0)
        {
            regs->rax = (uint64_t)-ENOENT;
            free(fpath);
            return 0;
        }
        node = vfs_open(fpath);
        if (!node)
        {
            regs->rax = (uint64_t)-ENOENT;
            free(fpath);
            return 0;
        }
    }

    if (node->type & file_dir)
    {
        vfs_close(node);
        regs->rax = (uint64_t)-EISDIR;
        free(fpath);
        return 0;
    }

    ssize_t ret = xfile_write_from_user(node, (void *)regs->rsi, regs->r9, regs->rdx);
    vfs_close(node);
    free(fpath);
    return ret;
}
