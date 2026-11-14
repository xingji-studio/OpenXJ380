#include <input_method.h>
#define ALL_IMPLEMENTATION
#include "errno.h"
#include "fs/fatfs/ff.h"
#include "krlibc.h"
#include "mutex.h"
#include "rtc.h"
#include <fs/vfs/vfs.h>
#include <syscall/syscall.h>
#include <block_access.h>
#include <cpu/regio.h>
#include <device.h>
#include <fs/fatfs/private.h>
#include <installer/esp.h>
#include <installer/fat32.h>
#include <efi/root_binding.h>
#include <task/scheduler.h>
#include <ioctl.h>
#include <rng.h>
#include <fs/fatfs/diskio.h>
#include <user/permissions.h>

static FATFS volume[10];
static int   fatfs_id = 0;
static const size_t FATFS_FASTSEEK_MIN_BYTES = 0x9000UL;
extern bool ahci_is_qemu_environment();
typedef struct file {
    char *path;
    void *handle;
    bool  is_dir;
} *file_t;

vfs_node_t drive_number_mapping[10] = {NULL};
static BlockAccessToken drive_leases[10];
static bool drive_prepared[10];

static int alloc_number() {
    for (int i = 0; i < 10; i++)
        if (drive_number_mapping[i] == NULL) return i;
    write_serial_fmt("No available drive number\n");
    return -1;
}

vfs_node_t fatfs_get_node_by_number(int number) {
    if (number < 0 || number >= 10) return NULL;
    return drive_number_mapping[number];
}
 
static mutex_t fatfs_operate_lock;
static bool fatfs_initialized, fatfs_poisoned;
static tcb_t fatfs_owner;
static size_t fatfs_depth;

static bool fatfs_has_failed()
{
    return __atomic_load_n(&fatfs_poisoned, __ATOMIC_ACQUIRE);
}

static void fatfs_poison()
{
    __atomic_store_n(&fatfs_poisoned, true, __ATOMIC_RELEASE);
    // Caller owns the FAT mutex. Do not clean-close or recycle cached state.
    for (BlockAccessToken lease : drive_leases) if (lease) block_access_poison(lease);
}

static inline bool fatfs_lock() {
    bool recovered = false;
    if (!__atomic_load_n(&fatfs_initialized, __ATOMIC_ACQUIRE) ||
        mutex_lock_recoverable(&fatfs_operate_lock, &recovered))
    {
        __atomic_store_n(&fatfs_poisoned, true, __ATOMIC_RELEASE);
        permissions_set_file_error(EIO);
        return false;
    }
    tcb_t task = get_current_task();
    if (recovered || (fatfs_depth && fatfs_owner != task))
    {
        fatfs_depth = 0;
        fatfs_owner = NULL;
        fatfs_poison();
    }
    if (fatfs_has_failed())
    {
        fatfs_poison();
        mutex_unlock(&fatfs_operate_lock);
        permissions_set_file_error(EIO);
        return false;
    }
    fatfs_owner = task;
    ++fatfs_depth;
    if (task) ++task->ime_storage_depth;
    return true;
}

static inline void fatfs_unlock() {
    tcb_t task = get_current_task();
    if (task) --task->ime_storage_depth;
    if (fatfs_owner == task && fatfs_depth && !--fatfs_depth) fatfs_owner = NULL;
    mutex_unlock(&fatfs_operate_lock);
}

void fatfs_quiescent_exit()
{
    // Reaper/task context, while the terminal mutex owner's TCB is still
    // pinned. Recovery only poisons state; it never resumes a partial request.
    if (!__atomic_load_n(&fatfs_initialized, __ATOMIC_ACQUIRE)) return;
    if (fatfs_lock()) fatfs_unlock();
}

int fatfs_factory_ready()
{
    if (!__atomic_load_n(&fatfs_initialized, __ATOMIC_ACQUIRE)) return -ENODEV;
    if (fatfs_has_failed()) return -EIO;
    tcb_t task = get_current_task();
    return task && mutex_get_owner(&fatfs_operate_lock) == task ? -EBUSY : 0;
}

int fatfs_set_serial(unsigned, void *, uint32_t);

int fatfs_format_node(vfs_node_t node)
{
    if (node == NULL) return -EINVAL;
    if (!fatfs_lock()) return -EIO;
    uint32_t serial;
    if (!get_identifier_random_bytes(&serial, sizeof(serial))) { fatfs_unlock(); return -EIO; }
    if (!serial) serial = 1;
    BlockExtent extent;
    BlockAccessToken lease = 0;
    int error = block_access_resolve_node(node, &extent);
    if (!error) error = block_access_require_flush(extent);
    if (!error) error = block_access_claim(extent, true, &lease);
    if (error) { fatfs_unlock(); return error; }
    int drive = alloc_number();
    if (drive < 0)
    {
        block_access_release(lease);
        fatfs_unlock();
        return -ENOSPC;
    }
    drive_number_mapping[drive] = node;
    drive_leases[drive] = lease;
    vfs_child_lock();
    if (node->refcount == uint32_t(-1))
    {
        vfs_child_unlock();
        drive_number_mapping[drive] = NULL;
        drive_leases[drive] = 0;
        block_access_release(lease);
        fatfs_unlock();
        return -EOVERFLOW;
    }
    ++node->refcount;
    vfs_child_unlock();
    char path[4];
    sprintf(path, "%d:", drive);
    MKFS_PARM opt;
    memset(&opt, 0, sizeof(opt));
    opt.fmt = FM_FAT32 | FM_SFD;
    opt.n_fat = 1;
    void *work = malloc(FF_MAX_SS);
    if (work == NULL)
    {
        drive_number_mapping[drive] = NULL;
        drive_leases[drive] = 0;
        block_access_release(lease);
        vfs_close(node);
        fatfs_unlock();
        return -ENOMEM;
    }
    // Test the actual durability path before the first formatter write.
    if (vfs_ioctl(node, IOBLKSYNC, NULL) != 0)
    {
        block_access_poison(lease);
        fatfs_unlock();
        permissions_set_file_error(EIO);
        return -EIO;
    }
    FRESULT res = f_mkfs(path, &opt, work, FF_MAX_SS);
    if (res != FR_OK || fatfs_set_serial(unsigned(drive), work, serial))
    {
        // Do not retry, invalidate, unmount or recycle an ambiguous format
        // request. Retain its buffer, device reference and poisoned lease.
        block_access_poison(lease);
        fatfs_unlock();
        return -EIO;
    }
    free(work);
    f_unmount(path);
    drive_number_mapping[drive] = NULL;
    drive_leases[drive] = 0;
    block_access_release(lease);
    vfs_close(node);
    fatfs_unlock();
    return res == FR_OK ? EOK : -EIO;
}

// FatFs supplies a time-derived default serial. Replace it in the adapter,
// including the backup BPB, without changing upstream formatter internals.
int fatfs_set_serial(unsigned drive, void *work, uint32_t serial)
{
    auto *sector = static_cast<byte *>(work);
    if (disk_read(byte(drive), sector, 0, 1) != RES_OK) return -EIO;
    unsigned reserved = unsigned(sector[14]) | unsigned(sector[15]) << 8;
    unsigned backup = unsigned(sector[50]) | unsigned(sector[51]) << 8;
    if (!serial || sector[510] != 0x55 || sector[511] != 0xaa ||
        memcmp(sector + 82, "FAT32   ", 8) || !backup || backup >= reserved) return -EIO;
    for (unsigned i = 0; i < 4; ++i) sector[67 + i] = byte(serial >> (8 * i));
    if (disk_write(byte(drive), sector, 0, 1) != RES_OK ||
        disk_write(byte(drive), sector, backup, 1) != RES_OK ||
        disk_ioctl(byte(drive), CTRL_SYNC, nullptr) != RES_OK) return -EIO;
    for (unsigned copy = 0; copy < 2; ++copy)
    {
        unsigned index = copy ? backup : 0;
        if (disk_read(byte(drive), sector, index, 1) != RES_OK) return -EIO;
        uint32_t actual = uint32_t(sector[67]) | uint32_t(sector[68]) << 8 |
                          uint32_t(sector[69]) << 16 | uint32_t(sector[70]) << 24;
        if (actual != serial) return -EIO;
    }
    return 0;
}

static u32 *fatfs_try_build_clmt(FIL *fp, size_t request_size) {
    if (!ahci_is_qemu_environment()) {
        return NULL;
    }

    if (fp == NULL || fp->obj.fs == NULL || fp->obj.sclust < 2 || request_size < FATFS_FASTSEEK_MIN_BYTES) {
        return NULL;
    }

    size_t bytes_per_cluster = (size_t)fp->obj.fs->csize * FF_MAX_SS;
    if (bytes_per_cluster == 0) {
        return NULL;
    }

    size_t cluster_count = ((size_t)fp->obj.objsize + bytes_per_cluster - 1) / bytes_per_cluster;
    if (cluster_count == 0) {
        cluster_count = 1;
    }

    if (cluster_count > (((size_t)-1) - 4) / 2) {
        return NULL;
    }

    size_t table_items = cluster_count * 2 + 4;
    if (table_items > 0xFFFFFFFFu) {
        return NULL;
    }

    u32 *clmt = (u32 *)malloc(table_items * sizeof(u32));
    if (clmt == NULL) {
        return NULL;
    }

    clmt[0] = (u32)table_items;
    return clmt;
}

int fatfs_mkdir(void *parent, const char *name, vfs_node_t node) {
    if (!fatfs_lock()) return -EIO;

    file_t p        = (file_t)parent;
    char  *new_path = (char*)malloc(strlen(p->path) + strlen((char *)name) + 1 + 1);
    sprintf(new_path, "%s/%s", p->path, name);
    FRESULT res = f_mkdir(new_path);
    if (res != FR_OK) 
    {
        if (res != FR_EXIST) write_serial_fmt("fatfs_mkdir: path=%s res=%d\n", new_path, res);
        free(new_path);
        fatfs_unlock();
        return res == FR_EXIST ? EOK : -EIO;
    }
    free(new_path);
    fatfs_unlock();
    return 0;
}

// f_stat on an SFN path can omit its long filename. Enumerate the root's
// directory entries, whose FILINFO contains both names, before any destructive
// open. Do this under fatfs_operate_lock, using FatFs directly (no VFS recursion).
static bool fatfs_name_equal(const char *a, const char *b);

static void fatfs_mark_ime_protection(vfs_node_t node, const char *path, const FILINFO *info)
{
    ime_vfs_resolved_name(node, info->fname);
    if (node->parent != rootdir) return;
    char *parent_path = strdup(path);
    if (!parent_path)
    {
        ime_vfs_resolved_name(node, "settings.json");
        return;
    }
    char *slash = strrchr(parent_path, '/');
    if (slash) *slash = 0;
    DIR directory;
    FILINFO entry;
    bool resolved = false;
    if (f_opendir(&directory, parent_path) == FR_OK)
    {
        while (f_readdir(&directory, &entry) == FR_OK && entry.fname[0])
        {
            if (fatfs_name_equal(node->name, entry.fname) || fatfs_name_equal(node->name, entry.altname))
            {
                ime_vfs_resolved_name(node, entry.fname);
                resolved = true;
                break;
            }
        }
        f_closedir(&directory);
    }
    // Fail closed when the existing file cannot be identified, rather than
    // allowing an unresolved alias to overwrite a protected root entry.
    if (!resolved) ime_vfs_resolved_name(node, "settings.json");
    free(parent_path);
}

int fatfs_mkfile(void *parent, const char *name, vfs_node_t node) {
    if (!fatfs_lock()) return -EIO;

    file_t p        = (file_t)parent;
    char  *new_path = (char*)malloc(strlen(p->path) + strlen((char *)name) + 1 + 1);
    sprintf(new_path, "%s/%s", p->path, name);
    FILINFO existing;
    if (f_stat(new_path, &existing) == FR_OK) fatfs_mark_ime_protection(node, new_path, &existing);
    if (!ime_vfs_access(node, NULL, IME_FILE_WRITE))
    {
        free(new_path); fatfs_unlock(); return -EACCES;
    }
    FIL fp;
    FRESULT res = f_open(&fp, new_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) 
    {
        int ret = -EIO;
        if (res == FR_DENIED || res == FR_EXIST) ret = -EACCES;
        else if (res == FR_NO_FILE || res == FR_NO_PATH) ret = -ENOENT;
        else if (res == FR_INVALID_NAME) ret = -EINVAL;
        else if (res == FR_NOT_ENOUGH_CORE) ret = -ENOMEM;
        write_serial_fmt("fatfs_mkfile: path=%s res=%d ret=%d\n", new_path, res, ret);
        free(new_path);
        fatfs_unlock();
        return ret;
    }
    res = f_close(&fp);
    if (res != FR_OK)
    {
        write_serial_fmt("fatfs_mkfile: close path=%s res=%d\n", new_path, res);
        free(new_path);
        fatfs_unlock();
        return -EIO;
    }
    free(new_path);
    fatfs_unlock();
    return 0;
}

size_t fatfs_readfile(file_t file, void *addr, size_t offset, size_t size) {
    if (!fatfs_lock()) return size_t(-1);
    
    if (file == NULL || addr == NULL) 
    {
        fatfs_unlock();
        return -1;
    }
    FIL    *fp        = (FIL *)file->handle;
    FRESULT res       = FR_OK;
    u32    *temp_clmt = NULL;

    if (fp == NULL) 
    {
        fatfs_unlock();
        return -1;
    }

    if (fp->cltbl == NULL) {
        temp_clmt = fatfs_try_build_clmt(fp, size);
        if (temp_clmt != NULL) {
            fp->cltbl = temp_clmt;
            res       = f_lseek(fp, CREATE_LINKMAP);
            if (res != FR_OK) {
                fp->cltbl = NULL;
                free(temp_clmt);
                temp_clmt = NULL;
                res       = FR_OK;
            }
        }
    }

    res = f_lseek(fp, offset);
    if (res != FR_OK) 
    {
        write_serial_fmt("fatfs_readfile: seek path=%s offset=%zu size=%zu res=%d\n",
                         file->path != NULL ? file->path : "<unknown>", offset, size, res);
        if (temp_clmt != NULL) {
            fp->cltbl = NULL;
            free(temp_clmt);
        }
        fatfs_unlock();
        return -1;
    }
    uint32_t n = 0;
    res = f_read(fp, addr, size, &n);
    if (temp_clmt != NULL) {
        fp->cltbl = NULL;
        free(temp_clmt);
    }
    if (res != FR_OK) 
    {
        write_serial_fmt("fatfs_readfile: read path=%s offset=%zu size=%zu got=%u res=%d\n",
                         file->path != NULL ? file->path : "<unknown>", offset, size, n, res);
        fatfs_unlock();
        return -1;
    }
    uint64_t file_size = (uint64_t)fp->obj.objsize;
    bool     reached_eof = (uint64_t)offset >= file_size ||
                       (uint64_t)n >= file_size - (uint64_t)offset;
    if (n != size && !reached_eof) {
        write_serial_fmt("fatfs_readfile: short read path=%s offset=%zu size=%zu got=%u res=%d\n",
                         file->path != NULL ? file->path : "<unknown>", offset, size, n, res);
    }
    fatfs_unlock();
    return n;
}

size_t fatfs_writefile(file_t file, const void *addr, size_t offset, size_t size) {
    if (!fatfs_lock()) return size_t(-1);
    
    if (file == NULL || addr == NULL) 
    {
        fatfs_unlock();
        return -1;
    }
    FIL *fp = (FIL *)file->handle;
    if (fp == NULL)
    {
        fatfs_unlock();
        return -1;
    }
    FRESULT res = FR_OK;
    if (fp->fptr != (FSIZE_t)offset)
    {
        res = f_lseek(fp, offset);
        if (res != FR_OK)
        {
            write_serial_fmt("fatfs_writefile: seek path=%s offset=%zu size=%zu res=%d\n",
                             file->path != NULL ? file->path : "<null>", offset, size, res);
            fatfs_unlock();
            return -1;
        }
    }
    uint32_t n = 0;
    res = f_write(fp, addr, size, &n);
    if (res == FR_OK)
    {
        FRESULT sync_res = f_sync(fp);
        if (sync_res != FR_OK)
        {
            write_serial_fmt("fatfs_writefile: sync path=%s offset=%zu size=%zu wrote=%u res=%d\n",
                             file->path != NULL ? file->path : "<null>", offset, size, n, sync_res);
            fatfs_unlock();
            return -1;
        }
    }
    if (res != FR_OK) 
    {
        write_serial_fmt("fatfs_writefile: write path=%s offset=%zu size=%zu wrote=%u res=%d\n",
                         file->path != NULL ? file->path : "<null>", offset, size, n, res);
        fatfs_unlock();
        return -1;
    }
    
    fatfs_unlock();
    return n;
}

static uint64_t ino = 2;

static bool fatfs_should_prune_child(vfs_node_t node) {
    if (node == NULL) return false;
    if (node->is_mount) return false;
    if (node->refcount > 1) return false;
    return (node->type & (file_none | file_dir)) != 0;
}

static uint64_t fatfs_time_to_ns(const FILINFO *fno) {
    if (fno == NULL || fno->fdate == 0) return realtime_ns();

    int year  = 1980 + ((fno->fdate >> 9) & 0x7f);
    int month = (fno->fdate >> 5) & 0x0f;
    int day   = fno->fdate & 0x1f;
    int hour  = (fno->ftime >> 11) & 0x1f;
    int min   = (fno->ftime >> 5) & 0x3f;
    int sec   = (fno->ftime & 0x1f) * 2;
    if (year < 1980 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        return realtime_ns();
    }

    tm time;
    memset(&time, 0, sizeof(time));
    time.tm_year = year;
    time.tm_mon  = month;
    time.tm_mday = day;
    time.tm_hour = hour;
    time.tm_min  = min;
    time.tm_sec  = sec;
    int64_t seconds = mktime(&time);
    if (seconds < 0) return realtime_ns();
    return (uint64_t)seconds * 1000000000ULL;
}

static int fatfs_ascii_lower(int ch) {
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static bool fatfs_name_equal(const char *a, const char *b) {
    if (a == NULL || b == NULL) return a == b;
    while (*a != '\0' && *b != '\0') {
        if (fatfs_ascii_lower((unsigned char)*a) != fatfs_ascii_lower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void fatfs_apply_filinfo(vfs_node_t node, const FILINFO *fno) {
    if (node == NULL || fno == NULL) return;
    uint64_t ns = fatfs_time_to_ns(fno);
    node->createtime = ns;
    node->readtime   = ns;
    node->writetime  = ns;
}

void fatfs_open(void *parent, const char *name, vfs_node_t node) {
    if (!fatfs_lock()) return;

    if (parent == NULL || name == NULL || node == NULL)
    {
        fatfs_unlock();
        return;
    }
    file_t p        = (file_t)parent;
    char  *new_path = (char*)malloc(strlen(p->path) + strlen((char *)name) + 1 + 1);
    file_t nw      = (file_t)malloc(sizeof(struct file));
    if (new_path == NULL || nw == NULL)
    {
        free(new_path);
        free(nw);
        fatfs_unlock();
        return;
    }
    sprintf(new_path, "%s/%s", p->path, name);
    void   *fp = NULL;
    FILINFO fno;
    FRESULT res = f_stat(new_path, &fno);
    if (res != FR_OK) {
        free(new_path);
        free(nw);
        fatfs_unlock();
        return;
    }
    fatfs_mark_ime_protection(node, new_path, &fno);
    fatfs_apply_filinfo(node, &fno);
    if (fno.fattrib & AM_DIR) {
        // node.
        node->type = file_dir;
        nw->is_dir = true;
        fp         = malloc(sizeof(DIR));
        if (fp == NULL)
        {
            free(new_path);
            free(nw);
            node->handle = NULL;
            fatfs_unlock();
            return;
        }
        res        = f_opendir((DIR*)fp, new_path);
        if (res != FR_OK)
        {
            free(fp);
            free(new_path);
            free(nw);
            node->handle = NULL;
            fatfs_unlock();
            return;
        }
        for (;;) {
            // 读取目录下的内容，再读会自动读下一个文件
            res = f_readdir((DIR*)fp, &fno);
            // 为空时表示所有项目读取完毕，跳出
            if (res != FR_OK || fno.fname[0] == 0) break;
            bool has_equre = false;
            vfs_child_lock();
            list_foreach(node->child, child_node0) {
                vfs_node_t e_child = (vfs_node_t)child_node0->data;
                if (fatfs_name_equal(e_child->name, fno.fname)) {
                    has_equre = true;
                    break;
                }
            }
            vfs_child_unlock();
            if (has_equre) continue;
            vfs_node_t child_node = vfs_child_append(node, fno.fname, NULL);
            child_node->type      = ((fno.fattrib & AM_DIR) != 0) ? file_dir : file_none;
            child_node->inode     = ino++;
            child_node->size      = fno.fsize;
            child_node->dev       = child_node->root->dev;
            fatfs_apply_filinfo(child_node, &fno);
        }
        if (node->inode == 0) node->inode = ino++;
        node->blksz = PAGE_SIZE;
    } else {
        node->type = file_none;
        nw->is_dir = false;
        fp         = malloc(sizeof(FIL));
        if (fp == NULL)
        {
            free(new_path);
            free(nw);
            node->handle = NULL;
            fatfs_unlock();
            return;
        }
        res = f_open((FIL*)fp, new_path, FA_READ | FA_WRITE);
        if (res != FR_OK)
        {
            write_serial_fmt("fatfs_open: f_open failed path=%s res=%d\n", new_path, (int)res);
            free(fp);
            free(new_path);
            free(nw);
            node->handle = NULL;
            node->size = 0;
            fatfs_unlock();
            return;
        }
        if (node->inode == 0) node->inode = ino++;
        node->size  = f_size((FIL *)fp);
        node->blksz = PAGE_SIZE;
        fatfs_apply_filinfo(node, &fno);
    }
    node->dev    = node->root->dev;
    nw->handle  = fp;
    nw->path    = new_path;
    node->handle = nw;

    fatfs_unlock();
}

bool fatfs_close(file_t handle) {
    if (!fatfs_lock()) return false;
    
    if (handle == NULL) {
        fatfs_unlock();
        return false;
    }

    FRESULT res;
    if (handle->is_dir) {
        res = f_closedir((DIR*)handle->handle);
    } else {
        res = f_close((FIL*)handle->handle);
    }
    if (res != FR_OK) 
    {
        fatfs_poison();
        permissions_set_file_error(EIO);
        fatfs_unlock();
        return false;
    }
    free(handle->path);
    free(handle->handle);
    free(handle);

    fatfs_unlock();
    return true;
}

static int fatfs_finalize_node(void *context, vfs_node_t)
{
    return fatfs_close(static_cast<file_t>(context)) ? 0 : -EIO;
}

static int fatfs_publish_cached_child(vfs_node_t parent, const FILINFO *info)
{
    vfs_node_t node = vfs_node_alloc(NULL, info->fname);
    if (!node) return -ENOMEM;
    node->parent = parent;
    node->root = parent->root;
    node->fsid = parent->fsid;
    node->dev = parent->root->dev;
    node->type = info->fattrib & AM_DIR ? file_dir : file_none;
    node->inode = ino++;
    node->size = info->fsize;
    node->visited = true;
    fatfs_apply_filinfo(node, info);
    list_t entry = list_alloc(node);
    bool publish = entry != NULL;
    vfs_child_lock();
    if (vfs_node_retiring_locked(parent)) publish = false;
    for (list_t old = parent->child; publish && old; old = old->next)
        if (fatfs_name_equal(((vfs_node_t)old->data)->name, node->name)) publish = false;
    if (publish)
    {
        entry->next = parent->child;
        if (entry->next) entry->next->prev = entry;
        parent->child = entry;
    }
    vfs_child_unlock();
    if (!publish)
    {
        free(entry);
        vfs_free(node); // Unpublished, reference-free private storage only.
    }
    return entry ? 0 : -ENOMEM;
}

errno_t fatfs_mount(const char *src, vfs_node_t node) {
    if (fatfs_has_failed()) { permissions_set_file_error(EIO); return -EIO; }
    if (is_virtual_fs(src)) return VFS_STATUS_FAILED;
    // The VFS authorized MOUNT before dispatch. Resolve the raw source with
    // no filesystem mutex held: a malicious path can traverse another FS.
    tcb_t lookup_task = get_current_task();
    if (lookup_task) ++lookup_task->ime_storage_depth;
    vfs_node_t device = vfs_open(src);
    if (lookup_task) --lookup_task->ime_storage_depth;
    BlockExtent extent;
    int access_error = device ? block_access_resolve_node(device, &extent) : -ENODEV;
    if (access_error)
    {
        if (device) vfs_close(device);
        return access_error;
    }
    if (!fatfs_lock()) { vfs_close(device); return -EIO; }
    int drive                   = alloc_number();
    if (drive < 0) { fatfs_unlock(); vfs_close(device); return -ENOSPC; }
    drive_number_mapping[drive] = device;
    access_error = block_access_claim(extent, false, &drive_leases[drive]);
    if (access_error)
    {
        vfs_close(drive_number_mapping[drive]);
        drive_number_mapping[drive] = NULL;
        fatfs_unlock();
        return access_error;
    }
    node->dev  = drive_number_mapping[drive]->rdev;
    char *path = (char*)malloc(3);
    if (!path)
    {
        vfs_close(drive_number_mapping[drive]);
        drive_number_mapping[drive] = NULL;
        block_access_release(drive_leases[drive]);
        drive_leases[drive] = 0;
        fatfs_unlock();
        return -ENOMEM;
    }
    sprintf(path, "%d:", drive);
    FRESULT r = f_mount(&volume[drive], path, 1);
    if (r != FR_OK) {
        if (r == FR_DISK_ERR || r == FR_NOT_READY)
        {
            block_access_poison(drive_leases[drive]);
            fatfs_unlock();
            return -EIO;
        }
        vfs_close(drive_number_mapping[drive]);
        drive_number_mapping[drive] = NULL;
        block_access_release(drive_leases[drive]);
        drive_leases[drive] = 0;
        free(path);
        fatfs_unlock();
        return -1;
    }
    file_t f = (file_t)malloc(sizeof(struct file));
    f->path  = path;
    f->is_dir = true;
    DIR *h   = (DIR*)malloc(sizeof(DIR));
    f_opendir(h, path);
    f->handle   = h;
    node->fsid  = fatfs_id;
    node->inode = 1;
    node->root  = node;
    uint64_t mounted_at = realtime_ns();
    node->createtime = mounted_at;
    node->readtime   = mounted_at;
    node->writetime  = mounted_at;

    FILINFO fno;
    FRESULT res;
    for (;;) {
        // 读取目录下的内容，再读会自动读下一个文件
        res = f_readdir(h, &fno);
        // 为空时表示所有项目读取完毕，跳出
        if (res != FR_OK || fno.fname[0] == 0) break;
        vfs_node_t exist = NULL;
        vfs_child_lock();
        list_foreach(node->child, child_node0) {
            vfs_node_t e_child = (vfs_node_t)child_node0->data;
            if (fatfs_name_equal(e_child->name, fno.fname)) {
                exist = e_child;
                break;
            }
        }
        vfs_child_unlock();
        if (exist) {
            exist->visited = true;
            fatfs_apply_filinfo(exist, &fno);
            continue;
        }
        vfs_node_t child_node = vfs_child_append(node, fno.fname, NULL);
        child_node->type      = ((fno.fattrib & AM_DIR) != 0) ? file_dir : file_none;
        child_node->inode     = ino++;
        child_node->size      = fno.fsize;
        child_node->visited   = true;
        child_node->dev       = child_node->root->dev;
        fatfs_apply_filinfo(child_node, &fno);
    }
    // node->inode  = ino++;
    node->handle = f;
    // node->blksz  = DEFAULT_PAGE_SIZE;
    
    fatfs_unlock();
    return VFS_STATUS_SUCCESS;
}

int fatfs_unmount_checked(void *root) {
    if (!fatfs_lock()) return -EIO;
    
    file_t f                     = (file_t)root;
    int    number                = f->path[0] - '0';
    bool prepared = drive_prepared[number];
    vfs_node_t device = drive_number_mapping[number];
    if (f_closedir((DIR*)f->handle) != FR_OK || f_unmount(f->path) != FR_OK)
    {
        fatfs_poison();
        fatfs_unlock();
        if (prepared) fatfs_unlock();
        permissions_set_file_error(EIO);
        return -EIO;
    }
    free(f->path);
    free(f->handle);
    free(f);
    drive_number_mapping[number] = NULL;
    block_access_release(drive_leases[number]);
    drive_leases[number] = 0;
    drive_prepared[number] = false;
    vfs_close(device);

    fatfs_unlock();
    if (prepared) fatfs_unlock(); // Consume the prepare phase's retained recursive level.
    return 0;
}

void fatfs_unmount(void *root) {
    (void)fatfs_unmount_checked(root);
}

int fatfs_stat(void *handle, vfs_node_t node) {
    if (!fatfs_lock()) return -EIO;

    file_t f = (file_t)handle;
    if (f == NULL || f->path == NULL || node == NULL || node->root == NULL)
    {
        fatfs_unlock();
        return -EINVAL;
    }
    vfs_child_lock();
    bool retiring = vfs_node_retiring_locked(node);
    vfs_child_unlock();
    if (retiring) { fatfs_unlock(); return -EBUSY; }
    FILINFO fno;
    FRESULT res = f_stat(f->path, &fno);
    if (res != FR_OK)
    {
        fatfs_unlock();
        return -1;
    }
    node->dev = node->root->dev;
    fatfs_apply_filinfo(node, &fno);
    if (!(fno.fattrib & AM_DIR))
    {
        node->type = file_none;
        node->size = fno.fsize;
        fatfs_unlock();
        return 0;
    }
    node->type = file_dir;
    DIR *fp = (DIR *)malloc(sizeof(DIR));
    if (fp == NULL) { fatfs_unlock(); return -ENOMEM; }
    res = f_opendir(fp, f->path);
    if (res != FR_OK)
    {
        free(fp);
        fatfs_unlock();
        return -1;
    }
    vfs_child_lock();
    list_foreach(node->child, child_node0)
        ((vfs_node_t)child_node0->data)->visited = false;
    vfs_child_unlock();

    int scan_error = 0;
    for (;;)
    {
        res = f_readdir(fp, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;
        vfs_node_t exist = NULL;
        bool retained_elsewhere = false;
        vfs_child_lock();
        list_foreach(node->child, child_node0)
        {
            vfs_node_t candidate = (vfs_node_t)child_node0->data;
            if (fatfs_name_equal(candidate->name, fno.fname))
            {
                retained_elsewhere = vfs_node_retiring_locked(candidate) || candidate->refcount == uint32_t(-1);
                if (!retained_elsewhere)
                {
                    ++candidate->refcount;
                    candidate->visited = true;
                    exist = candidate;
                }
                break;
            }
        }
        vfs_child_unlock();
        if (retained_elsewhere) continue;
        if (exist)
        {
            fatfs_apply_filinfo(exist, &fno);
            vfs_close(exist); // Drop this refresh pin; its tree reference remains.
        }
        else if ((scan_error = fatfs_publish_cached_child(node, &fno)) < 0) break;
    }
    FRESULT close_result = f_closedir(fp);
    free(fp);
    fatfs_unlock();
    // An incomplete scan is not evidence that any unseen cached name vanished.
    if (scan_error < 0) return scan_error;
    if (res != FR_OK || close_result != FR_OK) return -EIO;

    // Retiring a virtual-link subtree may close an ext4 orphan on another
    // volume. Do this only after releasing the FAT mutex. Limit the pass to
    // the captured child count so a concurrent refresh cannot cause a retry
    // loop over the same externally retained candidate.
    size_t remaining = 0;
    vfs_child_lock();
    list_foreach(node->child, child_node0) ++remaining;
    vfs_child_unlock();
    while (remaining--)
    {
        vfs_node_t candidate = NULL;
        vfs_child_lock();
        list_foreach(node->child, child_node0)
        {
            vfs_node_t child = (vfs_node_t)child_node0->data;
            if (!child->visited && fatfs_should_prune_child(child)) { candidate = child; break; }
        }
        vfs_child_unlock();
        if (!candidate) break;
        int error = vfs_prune_child(node, candidate, 0, true);
        if (error && error != -EBUSY) return error;
        if (error)
        {
            vfs_child_lock();
            list_foreach(node->child, child_node0)
                if (child_node0->data == candidate) { ((vfs_node_t)child_node0->data)->visited = true; break; }
            vfs_child_unlock();
        }
    }
    return 0;
}

int fatfs_delete(file_t parent, vfs_node_t node) {
    if (!fatfs_lock()) return -EIO;

    file_t file = (file_t)node->handle;
    if (file == NULL || file->path == NULL)
    {
        fatfs_unlock();
        return -EINVAL;
    }

    FRESULT res = f_unlink(file->path);
    
    fatfs_unlock();

    if (res == FR_DENIED) return -ENOTEMPTY;
    if (res != FR_OK) return -ENOENT;
    return VFS_STATUS_SUCCESS;
}

int fatfs_rename(file_t file, const char *nw) {
    if (!fatfs_lock()) return -EIO;
    if (file == NULL || file->path == NULL || nw == NULL) {
        fatfs_unlock();
        return VFS_STATUS_FAILED;
    }

    const char *target_path = nw;
    char       *fat_target  = NULL;
    if (nw[0] == '/' && file->path[0] != '\0' && file->path[1] == ':') {
        fat_target = (char *)malloc(strlen(nw) + 3);
        if (fat_target == NULL) {
            fatfs_unlock();
            return VFS_STATUS_FAILED;
        }
        fat_target[0] = file->path[0];
        fat_target[1] = ':';
        strcpy(fat_target + 2, nw);
        target_path = fat_target;
    }

    FRESULT res = f_rename((const char *)file->path, target_path);

    fatfs_unlock();
    free(fat_target);

    if (res != FR_OK) return -1;
    return 0;
}

static void fatfs_resize(file_t file, uint64_t size) {
    if (!fatfs_lock()) return;

    if (file == NULL || file->is_dir || file->handle == NULL) {
        fatfs_unlock();
        return;
    }

    FIL *fp = (FIL *)file->handle;
    if (fp->obj.objsize == 0 && size > 0 && f_expand(fp, (FSIZE_t)size, 1) == FR_OK) {
        if (f_lseek(fp, (FSIZE_t)size) == FR_OK) {
            f_truncate(fp);
        }
    } else if (fp->fptr == (FSIZE_t)size || f_lseek(fp, (FSIZE_t)size) == FR_OK) {
        f_truncate(fp);
    }

    fatfs_unlock();
}

int fatfs_ioctl(void *file, size_t cmd, void *arg) {
    if (!fatfs_lock()) return -EIO;
    fatfs_unlock();
    return -EOPNOTSUPP;
}

int fatfs_poll(void *file, size_t events) {
    if (!fatfs_lock()) return -EIO;
    fatfs_unlock();
    return -EOPNOTSUPP;
}

void *fatfs_map(void *file, void *addr, size_t offset, size_t size, size_t prot, size_t flags) {
    if (!fatfs_lock()) return NULL;
    void *result = general_map((vfs_read_t)fatfs_readfile, file, (uint64_t)addr, size, prot, flags, offset);
    fatfs_unlock();
    return result;
}

vfs_node_t fatfs_dup(vfs_node_t node) {
    if (!fatfs_lock()) return NULL;
    vfs_node_t copy   = vfs_node_alloc(node->parent, node->name);
    file_t     src    = (file_t)node->handle;
    file_t     tar    = (file_t)malloc(sizeof(struct file));
    tar->path         = strdup(src->path);
    tar->handle       = src->handle;
    tar->is_dir       = src->is_dir;
    copy->handle      = tar;
    copy->type        = node->type;
    copy->size        = node->size;
    copy->linkname    = node->linkname == NULL ? NULL : strdup(node->linkname);
    copy->flags       = node->flags;
    copy->ime_protection = node->ime_protection;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->child       = node->child;
    copy->realsize    = node->realsize;
    copy->inode       = node->inode;
    copy->dev         = node->dev;
    fatfs_unlock();
    return copy;
}

static int dummy() {
    if (!fatfs_lock()) return -EIO;
    fatfs_unlock();
    return -ENOSYS;
}

errno_t fatfs_statfs(vfs_node_t node, struct statfs *buf)
{
    if (node == NULL || buf == NULL) return -EINVAL;

    if (!fatfs_lock()) return -EIO;

    file_t file = (file_t)node->handle;
    const char *path = (file != NULL && file->path != NULL) ? file->path : "0:";
    u32    free_clusters = 0;
    FATFS *fs            = NULL;
    FRESULT res          = f_getfree(path, &free_clusters, &fs);
    if (res != FR_OK || fs == NULL)
    {
        fatfs_unlock();
        return -EIO;
    }

    uint64_t sector_size = FF_MAX_SS;
    uint64_t block_size  = (uint64_t)fs->csize * sector_size;
    if (block_size == 0) block_size = PAGE_SIZE;

    memset(buf, 0, sizeof(*buf));
    buf->f_type    = 0x4d44;
    buf->f_bsize   = block_size;
    buf->f_frsize  = block_size;
    buf->f_blocks  = fs->n_fatent > 2 ? fs->n_fatent - 2 : 0;
    buf->f_bfree   = free_clusters;
    buf->f_bavail  = free_clusters;
    buf->f_files   = 1024 * 1024;
    buf->f_ffree   = 1024 * 1024;
    buf->f_namelen = 255;

    fatfs_unlock();
    return EOK;
}

static int fatfs_prepare_unmount(void *context)
{
    if (!fatfs_lock()) return -EIO;
    file_t file = static_cast<file_t>(context);
    int drive = file && file->path && file->path[0] ? file->path[0] - '0' : -1;
    if (drive < 0 || drive >= 10 || file->path[1] != ':' || !drive_number_mapping[drive] || !drive_leases[drive])
    {
        fatfs_unlock();
        return -EINVAL;
    }
    if (drive_prepared[drive]) { fatfs_unlock(); return -EBUSY; }
    BlockExtent extent;
    int error = block_access_resolve_node(drive_number_mapping[drive], &extent);
    if (!error) error = block_access_require_flush(extent);
    if (error) { fatfs_unlock(); return error; }
    // Every successful write already f_syncs. This final physical flush is
    // fallible and runs before VFS is allowed to release any mounted vnode.
    error = vfs_ioctl(drive_number_mapping[drive], IOBLKSYNC, NULL);
    if (error) fatfs_poison();
    if (error)
    {
        fatfs_unlock();
        permissions_set_file_error(EIO);
        return -EIO;
    }
    // VFS quiescence excludes lookups/open references. Retain this recursive
    // level through cached-child close and final unmount; no other task can
    // enter FAT or poison its state between the durability gate and teardown.
    drive_prepared[drive] = true;
    return 0;
}

static struct vfs_callback fatfs_callbacks = {
    .mount    = fatfs_mount,
    .unmount  = fatfs_unmount,
    .open     = fatfs_open,
    .close    = (vfs_close_t)fatfs_close,
    .read     = (vfs_read_t)fatfs_readfile,
    .write    = (vfs_write_t)fatfs_writefile,
    .readlink = (vfs_readlink_t)dummy,
    .mkdir    = fatfs_mkdir,
    .mkfile   = fatfs_mkfile,
    .link     = (vfs_mk_t)dummy,
    .symlink  = (vfs_mk_t)dummy,
    .stat     = fatfs_stat,
    .ioctl    = fatfs_ioctl,
    .dup      = fatfs_dup,
    .poll     = fatfs_poll,
    .map      = (vfs_mapfile_t)fatfs_map,
    .resize   = (vfs_resize_t)fatfs_resize,
    .del      = (vfs_del_t)fatfs_delete,
    .rename   = (vfs_rename_t)fatfs_rename,
};

void fatfs_init() {
    if (__atomic_load_n(&fatfs_initialized, __ATOMIC_ACQUIRE)) return;
    mutex_create(&fatfs_operate_lock, true);
    __atomic_store_n(&fatfs_initialized, true, __ATOMIC_RELEASE);
    fatfs_id = vfs_regist("fatfs", &fatfs_callbacks, 0, 0x4d44);
    if (fatfs_id > 0)
    {
        vfs_set_prepare_unmount(fatfs_id, fatfs_prepare_unmount);
        vfs_set_unmount_quiescence(fatfs_id, true);
        vfs_set_checked_unmount(fatfs_id, fatfs_unmount_checked);
        vfs_set_finalize_node(fatfs_id, fatfs_finalize_node);
    }
    if (fatfs_id == VFS_STATUS_FAILED) { write_serial_fmt("Failed to register fat filesystem\n"); }
}

// Validate the selected bound FAT32 root before any user process can write it.
// A failure is fatal to this boot; the caller never falls back to another root.
int fatfs_mount_bound_root(const char *source, const XJ_ROOT_BINDING *binding)
{
    if (!source || !binding || !xj_root_binding_valid(binding) ||
        xj_root_binding_filesystem(binding) != XJ_ROOT_FILESYSTEM_FAT32) return -EINVAL;
    vfs_node_t device = vfs_open(source);
    if (!device) return -ENODEV;
    auto *buffer = static_cast<uint8_t *>(malloc(65536));
    if (!buffer) { vfs_close(device); return -ENOMEM; }
    int error = 0;
    if (vfs_read(device, buffer, 0, 512) != 512) error = -EIO;
    if (!error && (buffer[510] != 0x55 || buffer[511] != 0xaa ||
        memcmp(buffer + 82, "FAT32   ", 8) ||
        xj_root_binding_le32(buffer + 67) != xj_root_binding_fat32_id(binding))) error = -EUCLEAN;
    if (!error) error = vfs_mount(source, rootdir);
    if (!error)
    {
        vfs_node_t kernel = vfs_open("/system/kernel.krl");
        if (!kernel || kernel->size != xj_root_binding_kernel_bytes(binding)) error = -EUCLEAN;
        uint32_t crc = 0xffffffffU;
        for (uint64_t at = 0; !error && at < kernel->size;)
        {
            size_t count = kernel->size - at > 65536 ? 65536 : size_t(kernel->size - at);
            if (vfs_read(kernel, buffer, at, count) != count) { error = -EIO; break; }
            for (size_t i = 0; i < count; ++i)
            {
                crc ^= buffer[i];
                for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0x82f63b78U);
            }
            at += count;
        }
        if (!error && ~crc != xj_root_binding_le32(binding->bytes + 88)) error = -EUCLEAN;
        if (kernel && vfs_close(kernel)) error = -EIO;
    }
    free(buffer);
    if (vfs_close(device)) error = -EIO;
    return error;
}

// Original Apache-2.0 private factory ESP session. No valid root binding is
// written here: the coordinator must validate and activate the closed volume.
namespace {

struct EspContext
{
    BlockAccessWindow window;
    xjinstaller::EspPayload payload;
    xjinstaller::EspBuildStatus status;
    const xjpak2::Plan *root_plan;
    xjinstaller::ProgressObserver observer;
    uint8_t *seen;
    uint64_t data_total;
    uint64_t format_done, format_total;
    bool formatting;
    bool verifying;
    FATFS filesystem;
    FIL file;
    DIR directory;
    FILINFO information;
    MKFS_PARM mkfs;
    int error, drive;
    bool scratch_used[4];
    alignas(16) uint8_t scratch[4][32768];
    uint8_t mkfs_work[65536], stream[65536], placeholder[96];
    char path[520], name[512], directory_name[512];
    // Private root construction repeatedly scans dense FAT directories. Keep
    // a bounded, write-through sector cache; a fresh verifier gets a separately
    // zeroed context, so no writer bytes can satisfy independent readback.
    uint64_t root_cache_tags[2048];
    uint8_t root_cache[2048][512];
};

static EspContext *esp_contexts[10];
static EspContext *esp_active_allocator;

static bool esp_span(const void *pointer, size_t bytes)
{
    return pointer && bytes && uintptr_t(pointer) <= uintptr_t(-1) - (bytes - 1);
}

static bool esp_overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return uintptr_t(a) <= uintptr_t(b) ? uintptr_t(b) - uintptr_t(a) < an :
                                         uintptr_t(a) - uintptr_t(b) < bn;
}

static int esp_stop(EspContext *context, int error, bool io_failed = false)
{
    if (!context->error) context->error = error ? error : -EIO;
    if (io_failed) context->status.io_failed = true;
    if (context->status.io_started || context->status.phase >= xjinstaller::EspBuildPhase::Published)
    {
        context->status.quarantined = true;
        block_access_poison(context->window.lease);
    }
    return context->error;
}

static bool esp_same_extent(const BlockExtent &a, const BlockExtent &b)
{
    return a.device == b.device && a.physical_device == b.physical_device && a.generation == b.generation &&
           a.physical_generation == b.physical_generation && a.start == b.start && a.bytes == b.bytes;
}

static int esp_geometry(const BlockExtent &whole)
{
    // Call only after parent-token authentication: that lease prevents this
    // device-manager slot from being deleted or reused while it is inspected.
    device_t *device = get_device(whole.physical_device);
    if (!device || device->type != DEVICE_BLOCK || device->vdiskid != whole.device || device->size != whole.bytes)
        return -ENODEV;
    return device->sector_size == 512 ? 0 : -EOPNOTSUPP;
}

static int esp_validate(EspContext *context)
{
    if (context->error) return context->error;
    if (fatfs_has_failed() || !fatfs_depth || fatfs_owner != get_current_task())
        return esp_stop(context, -EIO);
    BlockExtent resolved;
    int error = block_access_resolve_node(context->window.node, &resolved);
    if (!error && (!esp_same_extent(resolved, context->window.whole) ||
                   context->window.node->size != resolved.bytes)) error = -ENODEV;
    if (!error) error = block_access_validate_owned_exclusive(context->window.lease, context->window.whole,
                                                              context->window.offset, context->window.bytes);
    if (!error) error = esp_geometry(context->window.whole);
    if (error) return esp_stop(context, error);
    return 0;
}

static int esp_fresult(EspContext *context, FRESULT result)
{
    if (result == FR_OK && !context->error) return 0;
    if (!context->status.fatfs_result) context->status.fatfs_result = uint32_t(result);
    return esp_stop(context, result == FR_NOT_ENOUGH_CORE ? -ENOMEM : -EIO);
}

static void esp_path(EspContext *context, const char *name)
{
    context->path[0] = char('0' + context->drive);
    context->path[1] = ':';
    strcpy(context->path + 2, name);
}

static int esp_file(EspContext *context, const char *name, const void *source, size_t bytes, bool write)
{
    esp_path(context, name);
    memset(&context->file, 0, sizeof(context->file));
    if (esp_fresult(context, f_open(&context->file, context->path,
                                   write ? (context->root_plan ? FA_CREATE_NEW : FA_CREATE_ALWAYS) | FA_WRITE : FA_READ)))
        return context->error;
    if (!write && f_size(&context->file) != bytes) return esp_stop(context, -EIO);
    size_t done = 0;
    while (done < bytes)
    {
        uint count = uint(bytes - done > sizeof(context->stream) ? sizeof(context->stream) : bytes - done);
        uint actual = 0;
        FRESULT result;
        if (write)
        {
            result = f_write(&context->file, static_cast<const uint8_t *>(source) + done, count, &actual);
            context->status.written_bytes += actual;
        }
        else
        {
            result = f_read(&context->file, context->stream, count, &actual);
        }
        if (esp_fresult(context, result) || actual != count)
            return esp_stop(context, -EIO);
        if (!write)
        {
            if (memcmp(context->stream, static_cast<const uint8_t *>(source) + done, count))
                return esp_stop(context, -EIO);
            context->status.verified_bytes += count;
        }
        done += count;
        if (context->root_plan)
            xjinstaller::report_progress(context->observer,
                write ? xjinstaller::InstallStage::Copying : xjinstaller::InstallStage::VerifyingRoot,
                write ? context->status.written_bytes : context->status.verified_bytes, context->data_total);
    }
    if (write && esp_fresult(context, f_sync(&context->file))) return context->error;
    return esp_fresult(context, f_close(&context->file));
}

static int esp_directory(EspContext *context, const char *name, const char *first, const char *second)
{
    esp_path(context, name);
    memset(&context->directory, 0, sizeof(context->directory));
    if (esp_fresult(context, f_opendir(&context->directory, context->path))) return context->error;
    unsigned found = 0;
    for (;;)
    {
        if (esp_fresult(context, f_readdir(&context->directory, &context->information))) return context->error;
        if (!context->information.fname[0]) break;
        unsigned bit = !strcmp(context->information.fname, first) ? 1 :
                       second && !strcmp(context->information.fname, second) ? 2 : 0;
        if (!bit || (found & bit)) return esp_stop(context, -EIO);
        found |= bit;
    }
    if (found != (second ? 3U : 1U)) return esp_stop(context, -EIO);
    return esp_fresult(context, f_closedir(&context->directory));
}

} // namespace

bool fatfs_esp_private_drive(unsigned drive)
{
    return drive < 10 && esp_contexts[drive];
}

int fatfs_esp_disk_status(unsigned drive)
{
    if (!fatfs_esp_private_drive(drive)) return -ENODEV;
    return esp_validate(esp_contexts[drive]);
}

static int esp_io(unsigned drive, void *buffer, uint64_t sector, unsigned count, bool write)
{
    if (!fatfs_esp_private_drive(drive)) return -ENODEV;
    EspContext *context = esp_contexts[drive];
    int error = esp_validate(context);
    if (error) return error;
    uint64_t sectors = context->window.bytes / 512;
    if (!buffer || !count || sector >= sectors || count > sectors - sector)
        return esp_stop(context, -EINVAL);
    size_t bytes = size_t(count) * 512;
    if (bytes / 512 != count) return esp_stop(context, -EOVERFLOW);
    uint64_t offset = context->window.offset + sector * 512;
    context->status.io_started = true;
    bool cached_root = context->root_plan && !context->formatting;
    if (cached_root && !write && count <= 2048)
    {
        bool hit = true;
        for (unsigned i = 0; i < count; ++i)
            if (context->root_cache_tags[(sector + i) & 2047] != sector + i + 1) { hit = false; break; }
        if (hit)
        {
            for (unsigned i = 0; i < count; ++i)
                memcpy(static_cast<uint8_t *>(buffer) + size_t(i) * 512,
                       context->root_cache[(sector + i) & 2047], 512);
            return 0;
        }
    }
    size_t actual = write ? vfs_write(context->window.node, buffer, offset, bytes) :
                            vfs_read(context->window.node, buffer, offset, bytes);
    if (actual != bytes) return esp_stop(context, -EIO, true);
    if (context->root_plan)
    {
        // Publish only after the complete backend request succeeds. Writes
        // never become deferred work; any short I/O poisons before reuse.
        for (unsigned i = 0; i < count; ++i)
        {
            memcpy(context->root_cache[(sector + i) & 2047],
                   static_cast<const uint8_t *>(buffer) + size_t(i) * 512, 512);
            context->root_cache_tags[(sector + i) & 2047] = sector + i + 1;
        }
    }
    if (write && context->root_plan && context->formatting)
    {
        // The first formatter write is its BPB. Derive the exact FAT/root
        // initialization work from that generated BPB, not volume capacity.
        if (!sector && bytes >= 512)
        {
            const auto *bpb = static_cast<const uint8_t *>(buffer);
            uint32_t fat_sectors = uint32_t(bpb[36]) | uint32_t(bpb[37]) << 8 |
                                   uint32_t(bpb[38]) << 16 | uint32_t(bpb[39]) << 24;
            context->format_total = (4ULL + uint64_t(bpb[16]) * fat_sectors + bpb[13]) * 512;
        }
        context->format_done += bytes;
        xjinstaller::report_progress(context->observer, xjinstaller::InstallStage::Formatting,
                                     context->format_done, context->format_total);
    }
    return 0;
}

int fatfs_esp_disk_read(unsigned drive, void *buffer, uint64_t sector, unsigned count)
{
    return esp_io(drive, buffer, sector, count, false);
}

int fatfs_esp_disk_write(unsigned drive, const void *buffer, uint64_t sector, unsigned count)
{
    return esp_io(drive, const_cast<void *>(buffer), sector, count, true);
}

int fatfs_esp_disk_sync(unsigned drive)
{
    if (!fatfs_esp_private_drive(drive)) return -ENODEV;
    EspContext *context = esp_contexts[drive];
    int error = esp_validate(context);
    if (error) return error;
    context->status.io_started = true;
    if (vfs_ioctl(context->window.node, IOBLKSYNC, NULL)) return esp_stop(context, -EIO, true);
    return 0;
}

uint32_t fatfs_esp_disk_sectors(unsigned drive)
{
    return fatfs_esp_disk_status(drive) ? 0 : uint32_t(esp_contexts[drive]->window.bytes / 512);
}

void *fatfs_esp_memalloc(unsigned bytes, bool *handled)
{
    EspContext *context = esp_active_allocator;
    *handled = context && fatfs_depth && fatfs_owner == get_current_task();
    if (!*handled) return NULL;
    if (!bytes || bytes > sizeof(context->scratch[0])) return NULL;
    for (unsigned i = 0; i < 4; ++i)
        if (!context->scratch_used[i])
        {
            context->scratch_used[i] = true;
            return context->scratch[i];
        }
    return NULL;
}

bool fatfs_esp_memfree(void *buffer)
{
    EspContext *context = esp_active_allocator;
    if (!context || !fatfs_depth || fatfs_owner != get_current_task()) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (buffer == context->scratch[i])
        {
            context->scratch_used[i] = false;
            return true;
        }
    return false;
}

static void fatfs_root_name(EspContext *context, const xjpak2::Span &path)
{
    memcpy(context->name, path.data, path.size);
    context->name[path.size] = 0;
}

static const xjpak2::Record *fatfs_root_find(EspContext *context, const char *name, size_t *index)
{
    size_t begin = 0, end = context->root_plan->count, length = strlen(name);
    while (begin < end)
    {
        size_t middle = begin + (end - begin) / 2;
        const auto &record = context->root_plan->records[middle];
        size_t common = length < record.path.size ? length : record.path.size;
        // Match the package parser's unsigned-byte ordering for UTF-8 paths.
        // The kernel memcmp currently orders signed chars, unlike hosted libc.
        int order = 0;
        for (size_t i = 0; i < common; ++i)
        {
            uint8_t value = uint8_t(name[i]);
            if (value == record.path.data[i]) continue;
            order = value < record.path.data[i] ? -1 : 1;
            break;
        }
        if (!order) order = length < record.path.size ? -1 : length > record.path.size ? 1 : 0;
        if (!order) { *index = middle; return &record; }
        if (order < 0) end = middle;
        else begin = middle + 1;
    }
    return nullptr;
}

static int fatfs_root_directory(EspContext *context, const char *name)
{
    esp_path(context, name);
    if (esp_fresult(context, f_opendir(&context->directory, context->path))) return context->error;
    const size_t prefix = strlen(name);
    for (;;)
    {
        if (esp_fresult(context, f_readdir(&context->directory, &context->information))) return context->error;
        if (!context->information.fname[0]) break;
        // Do not recurse through on-disk names: only enumerate directories
        // from the already validated manifest and match every returned child.
        size_t child = strlen(context->information.fname);
        size_t base = prefix == 1 ? 0 : prefix;
        if (base + 1 + child >= sizeof(context->name)) return esp_stop(context, -ENAMETOOLONG);
        memcpy(context->name, name, base);
        context->name[base] = '/';
        memcpy(context->name + base + 1, context->information.fname, child + 1);
        size_t index = 0;
        const auto *record = fatfs_root_find(context, context->name, &index);
        if (!record || context->seen[index] ||
            ((record->kind == xjpak2::Kind::Directory) != bool(context->information.fattrib & AM_DIR)) ||
            (record->kind == xjpak2::Kind::Regular && context->information.fsize != record->payload.size))
            return esp_stop(context, -EUCLEAN);
        context->seen[index] = 1;
    }
    return esp_fresult(context, f_closedir(&context->directory));
}

static int fatfs_root_file(EspContext *context, const xjpak2::Record &record, bool write)
{
    if (!record.payload_external)
        return esp_file(context, context->name, record.payload.data, record.payload.size, write);
    esp_path(context, context->name);
    memset(&context->file, 0, sizeof(context->file));
    if (esp_fresult(context, f_open(&context->file, context->path, write ? FA_CREATE_NEW | FA_WRITE : FA_READ)))
        return context->error;
    if (!write && f_size(&context->file) != record.payload.size) return esp_stop(context, -EUCLEAN);
    uint32_t source_crc = 0xffffffffU, target_crc = 0xffffffffU;
    for (size_t done = 0; done < record.payload.size;)
    {
        unsigned count = unsigned(record.payload.size - done > sizeof(context->stream) ? sizeof(context->stream) :
                                                                                       record.payload.size - done);
        int error = xjpak2::read_payload(*context->root_plan, record, done, context->mkfs_work, count);
        if (error) return esp_stop(context, error);
        source_crc = xjpak2::crc32c_update(source_crc, context->mkfs_work, count);
        unsigned actual = 0;
        FRESULT result = write ? f_write(&context->file, context->mkfs_work, count, &actual) :
                                 f_read(&context->file, context->stream, count, &actual);
        if (write) context->status.written_bytes += actual;
        else context->status.verified_bytes += actual;
        if (esp_fresult(context, result) || actual != count) return esp_stop(context, -EIO);
        if (!write)
        {
            if (memcmp(context->stream, context->mkfs_work, count)) return esp_stop(context, -EUCLEAN);
            target_crc = xjpak2::crc32c_update(target_crc, context->stream, count);
        }
        done += count;
        xjinstaller::report_progress(context->observer, write ? xjinstaller::InstallStage::Copying :
                                                                xjinstaller::InstallStage::VerifyingRoot,
             write ? context->status.written_bytes : context->status.verified_bytes, context->data_total);
    }
    if (~source_crc != record.payload_crc32c || (!write && ~target_crc != record.payload_crc32c))
        return esp_stop(context, -EUCLEAN);
    if (write && esp_fresult(context, f_sync(&context->file))) return context->error;
    return esp_fresult(context, f_close(&context->file));
}

static int fatfs_root_contents(EspContext *context, uint32_t volume_id, bool verify)
{
    using namespace xjinstaller;
    const auto &plan = *context->root_plan;
    esp_path(context, "");
    if (!verify)
    {
        if (fatfs_esp_disk_sync(context->drive)) return context->error;
        context->mkfs.fmt = FM_FAT32 | FM_SFD;
        context->mkfs.n_fat = 2;
        context->mkfs.align = 1;
        context->mkfs.au_size = fat32_cluster_bytes(context->window.bytes);
        context->formatting = true;
        report_progress(context->observer, InstallStage::Formatting, 0, 0);
        if (esp_fresult(context, f_mkfs(context->path, &context->mkfs, context->mkfs_work,
                                       sizeof(context->mkfs_work)))) return context->error;
        context->formatting = false;
        if (int error = fatfs_set_serial(context->drive, context->stream, volume_id))
            return esp_stop(context, error);
    }
    if (esp_fresult(context, f_mount(&context->filesystem, context->path, 1))) return context->error;
    if (context->filesystem.fs_type != FS_FAT32 || context->filesystem.n_fats != 2 ||
        fatfs_esp_disk_read(context->drive, context->stream, 0, 1)) return esp_stop(context, -EIO);
    uint32_t serial = uint32_t(context->stream[67]) | uint32_t(context->stream[68]) << 8 |
                      uint32_t(context->stream[69]) << 16 | uint32_t(context->stream[70]) << 24;
    if (serial != volume_id) return esp_stop(context, -EUCLEAN);
    if (verify)
    {
        if (fatfs_esp_disk_read(context->drive, context->mkfs_work, 6, 1)) return context->error;
        if (memcmp(context->stream, context->mkfs_work, 512)) return esp_stop(context, -EUCLEAN);
        // Read both FAT copies in bounded chunks before opening any files.
        uint32_t fat = context->filesystem.fsize;
        uint64_t first = context->filesystem.fatbase;
        for (uint32_t sector = 0; sector < fat;)
        {
            unsigned count = fat - sector > 64 ? 64 : fat - sector;
            if (fatfs_esp_disk_read(context->drive, context->stream, first + sector, count) ||
                fatfs_esp_disk_read(context->drive, context->mkfs_work, first + fat + sector, count))
                return context->error;
            if (memcmp(context->stream, context->mkfs_work, count * 512)) return esp_stop(context, -EUCLEAN);
            sector += count;
        }
    }
    report_progress(context->observer, verify ? InstallStage::VerifyingRoot : InstallStage::Directories, 0,
                     verify ? plan.data_bytes : plan.count);
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto &record = plan.records[i];
        if (record.path.size > 511) return esp_stop(context, -ENAMETOOLONG);
        if (record.kind != xjpak2::Kind::Directory || record.path.size == 1) continue;
        fatfs_root_name(context, record.path);
        if (!verify)
        {
            esp_path(context, context->name);
            if (esp_fresult(context, f_mkdir(context->path))) return context->error;
            report_progress(context->observer, InstallStage::Directories, i + 1, plan.count);
        }
    }
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto &record = plan.records[i];
        if (record.kind != xjpak2::Kind::Regular) continue;
        fatfs_root_name(context, record.path);
        if (fatfs_root_file(context, record, !verify)) return context->error;
    }
    if (verify)
    {
        if (fatfs_root_directory(context, "/")) return context->error;
        for (size_t i = 0; i < plan.count; ++i)
        {
            const auto &record = plan.records[i];
            if (record.kind == xjpak2::Kind::Directory && record.path.size > 1)
            {
                // path is separate from name, which enumeration uses as scratch.
                memcpy(context->path + 2, record.path.data, record.path.size);
                context->path[2 + record.path.size] = 0;
                char *directory = context->directory_name;
                memcpy(directory, context->path + 2, record.path.size + 1);
                if (fatfs_root_directory(context, directory)) return context->error;
            }
        }
        for (size_t i = 0; i < plan.count; ++i)
            if (plan.records[i].path.size != 1 && !context->seen[i]) return esp_stop(context, -EUCLEAN);
    }
    esp_path(context, "");
    if (esp_fresult(context, f_unmount(context->path))) return context->error;
    if (!verify && fatfs_esp_disk_sync(context->drive)) return context->error;
    report_progress(context->observer, verify ? InstallStage::VerifyingRoot : InstallStage::Copying,
                     plan.data_bytes, plan.data_bytes);
    return 0;
}

static int fatfs_factory_window(const BlockAccessWindow *window, const xjinstaller::EspPayload *payload,
                                const xjpak2::Plan *root_plan, uint32_t volume_id,
                                xjinstaller::EspBuildStatus *status,
                                const xjinstaller::ProgressObserver &observer, bool verify_only)
{
    if (!window || (!payload && !root_plan)) return -EINVAL;
    BlockAccessWindow selected = *window;
    xjinstaller::EspPayload source = payload ? *payload : xjinstaller::EspPayload{};
    xjinstaller::EspRequirements requirements;
    int error = root_plan ?
        (!root_plan->validated || (!root_plan->legacy_v1 && !root_plan->reader.read) ||
         !root_plan->count || !root_plan->records ||
         !volume_id || selected.bytes < 64ULL * 1048576 || selected.bytes % 512 ||
         selected.bytes / 512 >= 0x100000000ULL ? -EINVAL : 0) :
        xjinstaller::esp_plan(selected.bytes, source, requirements);
    if (error) return error;
    if (status && (!esp_span(status, sizeof(*status)) ||
                   esp_overlap(status, sizeof(*status), source.loader, source.loader_bytes) ||
                   esp_overlap(status, sizeof(*status), source.kernel, source.kernel_bytes))) return -EINVAL;
    if (status) *status = {};
    if (!selected.node || !(selected.node->type & file_block) || selected.offset % 512) return -EINVAL;
    if (!get_current_task() || !scheduler_is_enabled() || !(get_rflags() & (1ULL << 9))) return -EWOULDBLOCK;
    // A recursive private session would replace the outer session's scratch
    // allocator while FatFs still owns its embedded LFN buffers. Reject it
    // before permission checks can recurse into VFS. Only compare the lock's
    // short-lived owner snapshot; never dereference a foreign owner pointer.
    error = fatfs_factory_ready();
    if (error) return error;
    if (!vfs_check_access(selected.node, NULL, IME_FILE_MOUNT) ||
        !vfs_check_access(selected.node, NULL, IME_FILE_WRITE))
    {
        int error = vfs_last_error();
        return error ? -error : -EACCES;
    }
    BlockExtent resolved;
    error = block_access_resolve_node(selected.node, &resolved);
    if (!error && (!esp_same_extent(resolved, selected.whole) || selected.node->size != resolved.bytes))
        error = -ENODEV;
    if (!error) error = block_access_require_flush(resolved);
    if (!error)
        error = block_access_validate_owned_exclusive(selected.lease, selected.whole, selected.offset, selected.bytes);
    if (error) return error;
    // The retained whole lease prevents device re-registration. Byte-aligned
    // ranges alone cannot prevent a 4Kn backend from expanding an RMW request
    // beyond the window, so this factory profile requires real 512-byte I/O.
    error = esp_geometry(selected.whole);
    if (error) return error;
    if (!fatfs_lock()) return -EIO;
    if (fatfs_depth != 1 || esp_active_allocator)
    {
        fatfs_unlock();
        return -EBUSY;
    }
    auto *context = static_cast<EspContext *>(calloc(1, sizeof(EspContext)));
    if (!context) { fatfs_unlock(); return -ENOMEM; }
    context->window = selected;
    context->payload = source;
    context->root_plan = root_plan;
    context->observer = observer;
    context->verifying = verify_only;
    context->data_total = root_plan ? root_plan->data_bytes : 0;
    if (root_plan && verify_only)
    {
        context->seen = static_cast<uint8_t *>(calloc(root_plan->count, 1));
        if (!context->seen) { free(context); fatfs_unlock(); return -ENOMEM; }
    }
    context->status.phase = xjinstaller::EspBuildPhase::Preflight;
    context->drive = alloc_number();
    error = context->drive < 0 ? -ENOSPC : esp_validate(context);
    if (error) { free(context->seen); free(context); fatfs_unlock(); return error; }
    vfs_child_lock();
    bool overflow = selected.node->refcount == uint32_t(-1);
    if (!overflow) ++selected.node->refcount;
    vfs_child_unlock();
    if (overflow) { free(context->seen); free(context); fatfs_unlock(); return -EOVERFLOW; }
    // One heap block owns all FatFs objects, work buffers, and its bounded
    // allocator slabs. Publish it and the device pin before the first request.
    esp_contexts[context->drive] = context;
    drive_number_mapping[context->drive] = selected.node;
    drive_leases[context->drive] = selected.lease;
    fatfs_disk_cache_reset(context->drive);
    esp_active_allocator = context;
    context->status.phase = xjinstaller::EspBuildPhase::Published;
    do
    {
        if (root_plan)
        {
            if (fatfs_root_contents(context, volume_id, verify_only)) break;
            context->status.complete = true;
            context->status.phase = xjinstaller::EspBuildPhase::Complete;
            break;
        }
        if (fatfs_esp_disk_sync(context->drive)) break;
        context->status.phase = xjinstaller::EspBuildPhase::InitialFlush;
        esp_path(context, "");
        context->mkfs.fmt = FM_FAT32 | FM_SFD;
        context->mkfs.n_fat = 2;
        context->mkfs.align = 1;
        context->mkfs.au_size = 512;
        uint32_t serial;
        if (!get_identifier_random_bytes(&serial, sizeof(serial))) { esp_stop(context, -EIO); break; }
        if (!serial) serial = 1;
        if (esp_fresult(context, f_mkfs(context->path, &context->mkfs, context->mkfs_work,
                                       sizeof(context->mkfs_work)))) break;
        if (int error = fatfs_set_serial(context->drive, context->stream, serial))
        { esp_stop(context, error); break; }
        context->status.phase = xjinstaller::EspBuildPhase::Formatted;
        if (esp_fresult(context, f_mount(&context->filesystem, context->path, 1))) break;
        if (context->filesystem.fs_type != FS_FAT32) { esp_stop(context, -EIO); break; }
        context->status.phase = xjinstaller::EspBuildPhase::Mounted;
        const char *directories[] = {"/EFI", "/EFI/BOOT", "/EFI/XJ380", "/system"};
        for (const char *directory : directories)
        {
            esp_path(context, directory);
            if (esp_fresult(context, f_mkdir(context->path))) break;
        }
        if (context->error || esp_file(context, "/EFI/XJ380/root.bin", context->placeholder, 96, true) ||
            fatfs_esp_disk_sync(context->drive) ||
            esp_file(context, "/EFI/XJ380/root.bin", context->placeholder, 96, false)) break;
        context->status.phase = xjinstaller::EspBuildPhase::PlaceholderDurable;
        if (esp_file(context, "/EFI/BOOT/BOOTX64.EFI", source.loader, source.loader_bytes, true) ||
            esp_file(context, "/system/kernel.krl", source.kernel, source.kernel_bytes, true) ||
            fatfs_esp_disk_sync(context->drive)) break;
        context->status.phase = xjinstaller::EspBuildPhase::PayloadDurable;
        if (esp_file(context, "/EFI/BOOT/BOOTX64.EFI", source.loader, source.loader_bytes, false) ||
            esp_file(context, "/system/kernel.krl", source.kernel, source.kernel_bytes, false) ||
            esp_file(context, "/EFI/XJ380/root.bin", context->placeholder, 96, false) ||
            esp_directory(context, "/", "EFI", "system") || esp_directory(context, "/EFI", "BOOT", "XJ380") ||
            esp_directory(context, "/EFI/BOOT", "BOOTX64.EFI", NULL) ||
            esp_directory(context, "/EFI/XJ380", "root.bin", NULL) ||
            esp_directory(context, "/system", "kernel.krl", NULL)) break;
        context->status.phase = xjinstaller::EspBuildPhase::Readback;
        esp_path(context, "");
        if (esp_fresult(context, f_unmount(context->path))) break;
        context->status.phase = xjinstaller::EspBuildPhase::Closed;
        if (fatfs_esp_disk_sync(context->drive)) break;
        context->status.phase = xjinstaller::EspBuildPhase::Complete;
        context->status.complete = true;
    } while (false);
    error = context->error;
    if (!context->status.complete && !error) error = esp_stop(context, -EIO);
    if (status) *status = context->status;
    esp_active_allocator = NULL;
    if (error)
    {
        // Never unmount, close, free, recycle a slot or release the borrowed
        // parent after an ambiguous request or partially applied session.
        fatfs_unlock();
        permissions_set_file_error(-error);
        return error;
    }
    fatfs_disk_cache_reset(context->drive);
    esp_contexts[context->drive] = NULL;
    drive_number_mapping[context->drive] = NULL;
    drive_leases[context->drive] = 0;
    fatfs_unlock();
    vfs_close(selected.node);
    free(context->seen);
    free(context);
    return 0;
}

int fatfs_build_esp_window(const BlockAccessWindow *window, const xjinstaller::EspPayload *payload,
                           xjinstaller::EspBuildStatus *status)
{
    return fatfs_factory_window(window, payload, nullptr, 0, status, {}, false);
}

int xjinstaller::fatfs_build_root_window(const BlockAccessWindow *window, const xjpak2::Plan *plan,
                                         uint32_t volume_id, EspBuildStatus *status,
                                         const ProgressObserver &observer)
{
    return fatfs_factory_window(window, nullptr, plan, volume_id, status, observer, false);
}

int xjinstaller::fatfs_verify_root_window(const BlockAccessWindow *window, const xjpak2::Plan *plan,
                                          uint32_t volume_id, EspBuildStatus *status,
                                          const ProgressObserver &observer)
{
    return fatfs_factory_window(window, nullptr, plan, volume_id, status, observer, true);
}
