#include "../build_settings.h"
#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <elf.h>
#include <errno.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <rtc.h>
#include <stdint.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/user.h>
#include <user/runfile.h>
#include <cpu/lock.h>

char *current_user_envp[100] = {
    ENVP_SYSTEM_VERSION,
    (char *)"PATH=/usr/local/sbin:/usr/sbin:/sbin:/usr/bin:/bin:/apps:/system",//像不像
    NULL // 必须以NULL结尾
};

UserInfo *current_user = NULL;

UserInfo root_user = {
    .name = "Root",
    .user_type = XUT_Root,
    .envp = current_user_envp,
    .envc = 3,
};

static uint32_t stable_user_name_id(const char *name)
{
    uint32_t hash = 2166136261u;
    if (name == NULL || name[0] == '\0') return 1000;
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++)
    {
        hash ^= *p;
        hash *= 16777619u;
    }
    return 1000u + (hash % 60000u);
}

uint32_t user_uid(const UserInfo *user)
{
    if (user == NULL) return 0;
    switch (user->user_type)
    {
    case XUT_Root:
    case XUT_System:
    case XUT_Admin:
        return 0;
    case XUT_Visitor:
        return 65534;
    case XUT_Custom:
    default:
        return stable_user_name_id(user->name);
    }
}

uint32_t user_gid(const UserInfo *user)
{
    if (user == NULL) return 0;
    switch (user->user_type)
    {
    case XUT_Root:
    case XUT_System:
    case XUT_Admin:
        return 0;
    case XUT_Visitor:
        return 65534;
    case XUT_Custom:
    default:
        return user_uid(user);
    }
}

UserInfo *task_effective_user()
{
    tcb_t task = get_current_task();
    if (task != NULL && task->user_info != NULL) return task->user_info;
    if (current_user != NULL) return current_user;
    return &root_user;
}

extern bool no_interrupt;
extern bool is_scheduler;

static void restore_runtime_state(bool was_scheduler_enabled, bool was_interrupt_enabled)
{
    if (was_scheduler_enabled) enable_scheduler();
    if (was_interrupt_enabled && !no_interrupt) open_interrupt;
    else close_interrupt;
}

static bool append_cmdline_char(char *cmdline, size_t cmdline_size, char **cursor, char ch)
{
    if (cmdline == NULL || cursor == NULL || *cursor == NULL) return false;

    size_t used = (size_t)(*cursor - cmdline);
    if (used + 1 >= cmdline_size) return false;

    **cursor = ch;
    (*cursor)++;
    **cursor = '\0';
    return true;
}

static bool append_cmdline_arg(char *cmdline, size_t cmdline_size, char **cursor, const char *arg)
{
    if (cmdline == NULL || cursor == NULL || *cursor == NULL || arg == NULL) return false;

    bool quote = arg[0] == '\0';
    for (const char *p = arg; *p != '\0'; p++)
    {
        if (*p == ' ' || *p == '\'' || *p == '"' || *p == '\\')
        {
            quote = true;
            break;
        }
    }

    if (quote && !append_cmdline_char(cmdline, cmdline_size, cursor, '"')) return false;
    for (const char *p = arg; *p != '\0'; p++)
    {
        if (quote && (*p == '"' || *p == '\\') &&
            !append_cmdline_char(cmdline, cmdline_size, cursor, '\\'))
        {
            return false;
        }
        if (!append_cmdline_char(cmdline, cmdline_size, cursor, *p)) return false;
    }
    if (quote && !append_cmdline_char(cmdline, cmdline_size, cursor, '"')) return false;
    if (!append_cmdline_char(cmdline, cmdline_size, cursor, ' ')) return false;
    return true;
}

static bool user_registry_login_entry_valid(const UserInfo *user)
{
    if (user == NULL || user->name[0] == '\0') return false;
    if (memchr(user->name, '\0', sizeof(user->name)) == NULL ||
        memchr(user->password, '\0', sizeof(user->password)) == NULL) return false;
    if (user->user_type == XUT_Root || user->user_type == XUT_System) return false;
    if (user->user_type < XUT_Root || user->user_type > XUT_Custom) return false;
    for (const char *p = user->name; *p != '\0'; p++)
    {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == ' '))
            return false;
    }
    return true;
}

static bool user_registry_needs_oobe(UserRegisterList *urf_data, size_t bytes_read)
{
    if (urf_data == NULL) return true;
    if (bytes_read < sizeof(UserRegisterList)) return true;
    if (urf_data->user_count <= 1 || urf_data->user_count > 128) return true;
    for (int i = 1; i < urf_data->user_count; i++)
    {
        if (user_registry_login_entry_valid(&urf_data->uinf[i])) return false;
    }
    return true;
}

static bool user_registry_has_real_user(const UserRegisterList *registry)
{
    if (registry == NULL || registry->user_count <= 1 || registry->user_count > 128) return false;
    for (int i = 1; i < registry->user_count; i++)
    {
        if (user_registry_login_entry_valid(&registry->uinf[i])) return true;
    }
    return false;
}

static bool user_registry_first_user_matches(const UserRegisterList *registry, const char *username,
                                             const char *password)
{
    if (registry == NULL || username == NULL || password == NULL) return false;
    if (registry->user_count <= 1 || registry->user_count > 128) return false;

    for (int i = 1; i < registry->user_count; i++)
    {
        if (!user_registry_login_entry_valid(&registry->uinf[i])) continue;
        return strcmp(username, registry->uinf[i].name) == 0 && strcmp(password, registry->uinf[i].password) == 0;
    }
    return false;
}

static bool load_user_registry(UserRegisterList *registry, size_t *bytes_read)
{
    if (registry == NULL) return false;
    memset(registry, 0, sizeof(UserRegisterList));
    if (bytes_read != NULL) *bytes_read = 0;

    vfs_node_t node = vfs_open("/system/config/usereg.dat");
    if (node == NULL) return false;

    size_t read_size = vfs_read(node, registry, 0, sizeof(UserRegisterList));
    vfs_close(node);
    if (bytes_read != NULL) *bytes_read = read_size;
    return read_size >= sizeof(int);
}

static bool write_first_user_registry(const char *username, const char *password)
{
    if (username == NULL || password == NULL) return false;

    vfs_mkdir("/system");
    vfs_mkdir("/system/config");

    vfs_node_t file = vfs_open("/system/config/usereg.dat");
    if (file == NULL)
    {
        if (vfs_mkfile("/system/config/usereg.dat") != EOK) return false;
        file = vfs_open("/system/config/usereg.dat");
    }
    if (file == NULL) return false;

    UserRegisterList registry;
    memset(&registry, 0, sizeof(registry));
    registry.user_count = 2;
    strcpy(registry.uinf[0].name, "Root");
    strcpy(registry.uinf[0].password, "");
    registry.uinf[0].user_type = XUT_Root;
    strncpy(registry.uinf[1].name, username, sizeof(registry.uinf[1].name) - 1);
    strncpy(registry.uinf[1].password, password, sizeof(registry.uinf[1].password) - 1);
    registry.uinf[1].user_type = XUT_Admin;

    vfs_resize(file, 0);
    size_t wrote = vfs_write(file, &registry, 0, sizeof(registry));
    vfs_close(file);
    return wrote == sizeof(registry);
}

static void set_current_user_from_info(UserInfo *info)
{
    if (info == NULL) return;
    if (current_user == NULL) current_user = (UserInfo *)malloc(sizeof(UserInfo));
    if (current_user == NULL) return;
    memset(current_user, 0, sizeof(UserInfo));
    strcpy(current_user->name, info->name);
    strcpy(current_user->password, info->password);
    current_user->user_type = info->user_type;
    current_user->envc      = 3;
    current_user->envp      = current_user_envp;
}

void user_session_use_root()
{
    set_current_user_from_info(&root_user);
}

void user_session_use_login()
{
    UserInfo login_user;
    memset(&login_user, 0, sizeof(login_user));
    strcpy(login_user.name, "Login");
    login_user.user_type = XUT_Visitor;
    set_current_user_from_info(&login_user);
}

bool user_session_needs_oobe()
{
    UserRegisterList registry;
    size_t           bytes_read = 0;
    load_user_registry(&registry, &bytes_read);
    return user_registry_needs_oobe(&registry, bytes_read);
}

int user_session_list(UserInfo *out, int max_count)
{
    if (out == NULL || max_count < 0) return -EINVAL;

    UserRegisterList registry;
    size_t           bytes_read = 0;
    load_user_registry(&registry, &bytes_read);
    if (user_registry_needs_oobe(&registry, bytes_read)) return 0;

    int copied = 0;
    for (int i = 1; i < registry.user_count && i < 128; i++)
    {
        if (!user_registry_login_entry_valid(&registry.uinf[i])) continue;
        if (copied >= max_count) break;
        out[copied] = registry.uinf[i];
        memset(out[copied].password, 0, sizeof(out[copied].password));
        copied++;
    }
    return copied;
}

int user_session_login(const char *username, const char *password)
{
    if (username == NULL || password == NULL || username[0] == '\0') return -EINVAL;

    static spin_t login_lock = SPIN_INIT;
    static uint64_t retry_after_ns = 0;
    static unsigned failed_attempts = 0;
    spin_lock(&login_lock);
    uint64_t now = nanoTime();
    if (now < retry_after_ns) { spin_unlock(&login_lock); return -EAGAIN; }
    spin_unlock(&login_lock);

    UserRegisterList registry;
    size_t           bytes_read = 0;
    load_user_registry(&registry, &bytes_read);
    if (user_registry_needs_oobe(&registry, bytes_read)) return -ENOENT;

    for (int i = 1; i < registry.user_count && i < 128; i++)
    {
        if (!user_registry_login_entry_valid(&registry.uinf[i])) continue;
        if (strcmp(username, registry.uinf[i].name) != 0) continue;
        if (strcmp(password, registry.uinf[i].password) != 0)
        {
            spin_lock(&login_lock);
            failed_attempts++;
            uint64_t delay_seconds = 1ULL << min(failed_attempts, 5U);
            retry_after_ns = now + delay_seconds * 1000000000ULL;
            spin_unlock(&login_lock);
            return -EACCES;
        }

        set_current_user_from_info(&registry.uinf[i]);
        spin_lock(&login_lock);
        failed_attempts = 0;
        retry_after_ns = 0;
        spin_unlock(&login_lock);
        if (current_user != NULL) init_user_profile(current_user->name);
        return 0;
    }
    spin_lock(&login_lock);
    failed_attempts++;
    retry_after_ns = now + (1ULL << min(failed_attempts, 5U)) * 1000000000ULL;
    spin_unlock(&login_lock);
    return -ENOENT;
}

int user_session_create_first(const char *username, const char *password)
{
    if (username == NULL || username[0] == '\0' || password == NULL || password[0] == '\0') return -EINVAL;
    for (const char *p = username; *p != '\0'; p++)
    {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == ' '))
            return -EINVAL;
    }

    UserRegisterList registry;
    size_t           bytes_read = 0;
    bool registry_loaded = load_user_registry(&registry, &bytes_read);
    vfs_node_t existing_registry = vfs_open("/system/config/usereg.dat");
    bool registry_exists = existing_registry != NULL;
    if (existing_registry != NULL) vfs_close(existing_registry);
    if (registry_exists && (!registry_loaded || bytes_read != sizeof(UserRegisterList) ||
                            registry.user_count < 1 || registry.user_count > 128)) return -EIO;
    if (!user_registry_needs_oobe(&registry, bytes_read))
    {
        if (user_registry_first_user_matches(&registry, username, password))
        {
            UserInfo *first_user = NULL;
            for (int i = 1; i < registry.user_count && i < 128; i++)
            {
                if (user_registry_login_entry_valid(&registry.uinf[i]))
                {
                    first_user = &registry.uinf[i];
                    break;
                }
            }
            if (first_user != NULL)
            {
                set_current_user_from_info(first_user);
                if (current_user != NULL) init_user_profile(current_user->name);
                return 0;
            }
        }
        return -EEXIST;
    }

    if (user_registry_has_real_user(&registry)) return -EEXIST;
    if (!write_first_user_registry(username, password) || !init_user_profile(username)) return -EIO;

    UserInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name, username, sizeof(info.name) - 1);
    strncpy(info.password, password, sizeof(info.password) - 1);
    info.user_type = XUT_Admin;
    set_current_user_from_info(&info);
    return 0;
}

void init_user()
{
}

void copy_args(char *dst[], char *src[], int n)
{
    for (int i = 0; i < n; i++)
    {
        dst[i] = NULL;
        if (src[i] != NULL)
        {
            size_t len = strlen(src[i]);
            dst[i] = (char *)malloc(len + 1);
            if (dst[i] == NULL)
            {
                for (int j = 0; j < i; ++j)
                {
                    if (dst[j] != NULL)
                    {
                        free(dst[j]);
                        dst[j] = NULL;
                    }
                }
                return;
            }
            memcpy(dst[i], src[i], len + 1);
        }
    }
}

void get_thread_name_from_filepath(char *path, char *name)
{
    if (path == NULL || name == NULL)
    {
        return;
    }

    char *start = path;
    for (char *p = path; *p != '\0'; p++)
    {
        if (*p == '\\' || *p == '/' || *p == '|') start = p + 1;
    }

    size_t i = 0;
    while (start[i] != '\0' && start[i] != '.' && i < 31)
    {
        name[i] = start[i];
        i++;
    }
    name[i] = '\0';
}
void *aligned_malloc(size_t size, size_t alignment)
{
    // 分配额外空间用于存储原始指针
    size_t extra    = alignment - 1 + sizeof(void *);
    void  *original = malloc(size + extra);

    if (!original) return nullptr;

    // 计算对齐后的地址
    void *aligned = (void *)(((uintptr_t)original + sizeof(void *) + alignment - 1) & ~(alignment - 1));

    // 在aligned指针前保存original指针
    ((void **)aligned)[-1] = original;

    return aligned;
}

#define USER_DYN_MAIN_BASE 0x0000000040000000UL
#define USER_INTERP_BASE   0x0000000060000000UL

typedef struct loaded_user_elf
{
    uint64_t map_start;
    uint64_t map_end;
    uint64_t load_bias;
    uint64_t entry;
    uint64_t phdr;
    uint64_t phent;
    uint64_t phnum;
} loaded_user_elf_t;

static void free_file_image(uint8_t *buf, uint64_t size)
{
    if (buf == NULL || size == 0) return;
    free_frames((uint64_t)virt_to_phys((uint64_t)buf), (size + PAGE_SIZE - 1) / PAGE_SIZE);
}

static int read_vfs_file(vfs_node_t file, uint8_t **out, uint64_t *out_size)
{
    if (file == NULL || out == NULL || out_size == NULL) return -EINVAL;
    uint64_t size = file->size;
    if (size == 0) return -ENOEXEC;

    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = alloc_frames(pages);
    if (phys == 0) return -ENOMEM;
    uint8_t *buf = (uint8_t *)phys_to_virt(phys);
    if (buf == NULL) return -ENOMEM;
    memset(buf, 0, pages * PAGE_SIZE);

    size_t got = vfs_read(file, buf, 0, size);
    if (got != size)
    {
        free_file_image(buf, size);
        return -EIO;
    }

    *out = buf;
    *out_size = size;
    return 0;
}

static bool elf_range_in_file(uint64_t offset, uint64_t size, uint64_t file_size)
{
    return offset <= file_size && size <= file_size - offset;
}

static int find_elf_interpreter(uint8_t *buf, uint64_t file_size, char *interp, size_t interp_size)
{
    if (buf == NULL || interp == NULL || interp_size == 0) return -EINVAL;
    interp[0] = '\0';

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    if (!elf_range_in_file(ehdr->e_phoff, (uint64_t)ehdr->e_phnum * ehdr->e_phentsize, file_size)) return -ENOEXEC;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(buf + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type != PT_INTERP) continue;
        if (phdrs[i].p_filesz == 0 || phdrs[i].p_filesz >= interp_size) return -ENAMETOOLONG;
        if (!elf_range_in_file(phdrs[i].p_offset, phdrs[i].p_filesz, file_size)) return -ENOEXEC;

        memcpy(interp, buf + phdrs[i].p_offset, phdrs[i].p_filesz);
        interp[phdrs[i].p_filesz] = '\0';
        return 0;
    }

    return 0;
}

static int load_user_elf_image(uint8_t *buf, uint64_t file_size, pcb_t group, const char *name,
                               uint64_t dyn_base, loaded_user_elf_t *out)
{
    if (buf == NULL || group == NULL || out == NULL) return -EINVAL;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    if (!elf_range_in_file(0, sizeof(Elf64_Ehdr), file_size) ||
        (*(uint32_t *)ehdr->e_ident != 0x464C457F) ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phnum == 0 ||
        ehdr->e_phnum > file_size / sizeof(Elf64_Phdr) ||
        !elf_range_in_file(ehdr->e_phoff, (uint64_t)ehdr->e_phnum * ehdr->e_phentsize, file_size))
    {
        write_serial_fmt("ELF load reject %s: size=%llu magic=%02x %02x %02x %02x type=%u machine=%u phoff=%llu phnum=%u phentsize=%u\n",
                         name != NULL ? name : "(null)",
                         file_size,
                         file_size > 0 ? buf[0] : 0,
                         file_size > 1 ? buf[1] : 0,
                         file_size > 2 ? buf[2] : 0,
                         file_size > 3 ? buf[3] : 0,
                         elf_range_in_file(0, sizeof(Elf64_Ehdr), file_size) ? ehdr->e_type : 0,
                         elf_range_in_file(0, sizeof(Elf64_Ehdr), file_size) ? ehdr->e_machine : 0,
                         elf_range_in_file(0, sizeof(Elf64_Ehdr), file_size) ? ehdr->e_phoff : 0,
                         elf_range_in_file(0, sizeof(Elf64_Ehdr), file_size) ? ehdr->e_phnum : 0,
                         elf_range_in_file(0, sizeof(Elf64_Ehdr), file_size) ? ehdr->e_phentsize : 0);
        return -ENOEXEC;
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
    {
        write_serial_fmt("ELF load reject %s: unsupported type=%u\n", name != NULL ? name : "(null)", ehdr->e_type);
        return -ENOEXEC;
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(buf + ehdr->e_phoff);
    uint64_t load_bias = ehdr->e_type == ET_DYN ? dyn_base : 0;
    uint64_t load_start = ~0ULL;
    uint64_t load_end = 0;
    bool entry_is_executable = false;

    for (int i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_filesz > phdrs[i].p_memsz ||
            !elf_range_in_file(phdrs[i].p_offset, phdrs[i].p_filesz, file_size))
        {
            write_serial_fmt("ELF load reject %s: bad PT_LOAD index=%d offset=%llu filesz=%llu memsz=%llu file_size=%llu\n",
                             name != NULL ? name : "(null)",
                             i,
                             phdrs[i].p_offset,
                             phdrs[i].p_filesz,
                             phdrs[i].p_memsz,
                             file_size);
            return -ENOEXEC;
        }

        if (phdrs[i].p_vaddr > ~0ULL - load_bias) return -ENOEXEC;
        uint64_t seg_start = load_bias + phdrs[i].p_vaddr;
        if (phdrs[i].p_memsz == 0 || check_user_overflow(seg_start, phdrs[i].p_memsz) || seg_start < PAGE_SIZE)
            return -ENOEXEC;
        uint64_t seg_end = seg_start + phdrs[i].p_memsz;
        if ((phdrs[i].p_flags & PF_X) != 0 && ehdr->e_entry <= ~0ULL - load_bias)
        {
            uint64_t entry = load_bias + ehdr->e_entry;
            if (entry >= seg_start && entry < seg_end) entry_is_executable = true;
        }
        load_start = min(load_start, (uint64_t)PADDING_DOWN(seg_start, PAGE_SIZE));
        load_end = max(load_end, (uint64_t)PADDING_UP(seg_end, PAGE_SIZE));
    }

    static constexpr uint64_t MAX_USER_ELF_SPAN = 1ULL << 30;
    if (load_start == ~0ULL || load_end <= load_start || load_end - load_start > MAX_USER_ELF_SPAN ||
        check_user_overflow(load_start, load_end - load_start) || !entry_is_executable)
    {
        write_serial_fmt("ELF load reject %s: no loadable segments\n", name != NULL ? name : "(null)");
        return -ENOEXEC;
    }

    page_map_range_to_random(group->pagedir, load_start, load_end - load_start,
                             PTE_PRESENT | PTE_WRITEABLE | PTE_USER);
    memset((void *)load_start, 0, load_end - load_start);

    uint64_t phdr_addr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type == PT_PHDR)
        {
            phdr_addr = load_bias + phdrs[i].p_vaddr;
            break;
        }
    }
    for (int i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_addr = load_bias + phdrs[i].p_vaddr;
        memcpy((void *)seg_addr, (void *)(buf + phdrs[i].p_offset), phdrs[i].p_filesz);
        if (phdrs[i].p_memsz > phdrs[i].p_filesz)
        {
            memset((void *)(seg_addr + phdrs[i].p_filesz), 0, phdrs[i].p_memsz - phdrs[i].p_filesz);
        }

        if (phdr_addr == 0 && ehdr->e_phoff >= phdrs[i].p_offset &&
            ehdr->e_phoff < phdrs[i].p_offset + phdrs[i].p_filesz)
        {
            phdr_addr = seg_addr + (ehdr->e_phoff - phdrs[i].p_offset);
        }
    }

    vma_t *elf_vma = vma_alloc();
    if (elf_vma != NULL)
    {
        elf_vma->vm_start = load_start;
        elf_vma->vm_end = load_end;
        elf_vma->vm_flags |= VMA_READ | VMA_WRITE | VMA_EXEC;
        elf_vma->vm_type = VMA_TYPE_ANON;
        elf_vma->vm_name = strdup(name != NULL ? name : "elf");
        vma_insert(&group->vma_manager, elf_vma);
    }

    out->map_start = load_start;
    out->map_end = load_end;
    out->load_bias = load_bias;
    out->entry = load_bias + ehdr->e_entry;
    out->phdr = phdr_addr;
    out->phent = ehdr->e_phentsize;
    out->phnum = ehdr->e_phnum;
    return 0;
}

uint64_t parse_elf_file(char *path, pcb_t group)
{
    if (group == NULL)
    {
        pr_warn("Parse ELF/EPF file failed.\n");
        write_serial_fmt("Reason:   ? Where is your parent group?\n");
        return NULL;
    }

    vfs_node_t file = vfs_open(path);
    if (file == NULL) return NULL;

    uint8_t *buf = NULL;
    uint64_t file_size = 0;
    int ret = read_vfs_file(file, &buf, &file_size);
    vfs_close(file);
    if (ret < 0)
    {
        pr_warn("Parse ELF/EPF file failed.\n");
        write_serial_fmt("Reason:   Cannot Read File: %s, ret=%d.\n", path, ret);
        return (uint64_t)ret;
    }

    if (file_size < sizeof(Elf64_Ehdr))
    {
        free_file_image(buf, file_size);
        write_serial_fmt("Parse ELF/EPF file failed. Reason: Truncated header: %s.\n", path);
        return (uint64_t)-ENOEXEC;
    }
    Elf64_Ehdr ehdr = *(Elf64_Ehdr *)buf;

    // 验证ELF/EPF魔数 (0x7F,'E','L','F') (0x24, 'E', 'P', 'F')
    if ((*(uint32_t *)ehdr.e_ident != 0x464C457F)     // ELF
        && (*(uint32_t *)ehdr.e_ident != 0x46504524)) // EPF
    {
        pr_warn("Parse ELF/EPF file failed.\n");
        write_serial_fmt("Reason:   Bad Format path=%s size=%llu magic=%02x %02x %02x %02x.\n",
                         path,
                         file_size,
                         file_size > 0 ? buf[0] : 0,
                         file_size > 1 ? buf[1] : 0,
                         file_size > 2 ? buf[2] : 0,
                         file_size > 3 ? buf[3] : 0);
        free_file_image(buf, file_size);
        return NULL;
    }

    char interp_path[256];
    ret = find_elf_interpreter(buf, file_size, interp_path, sizeof(interp_path));
    if (ret < 0)
    {
        free_file_image(buf, file_size);
        write_serial_fmt("Parse ELF file failed. Reason: Bad PT_INTERP in %s, ret=%d\n", path, ret);
        return (uint64_t)ret;
    }

    uint8_t *interp_buf = NULL;
    uint64_t interp_size = 0;
    if (interp_path[0] != '\0')
    {
        vfs_node_t interp_file = vfs_open(interp_path);
        if (interp_file == NULL)
        {
            free_file_image(buf, file_size);
            write_serial_fmt("Parse ELF file failed. Reason: Interpreter not found: %s\n", interp_path);
            return (uint64_t)-ENOENT;
        }
        ret = read_vfs_file(interp_file, &interp_buf, &interp_size);
        vfs_close(interp_file);
        if (ret < 0)
        {
            free_file_image(buf, file_size);
            write_serial_fmt("Parse ELF file failed. Reason: Cannot read interpreter %s, ret=%d\n", interp_path, ret);
            return (uint64_t)ret;
        }
    }

    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    if (!no_interrupt) close_interrupt;
    disable_scheduler();

    page_directory_t *current_pagedir = get_current_directory();
    switch_page_directory(group->pagedir);

    loaded_user_elf_t main_elf;
    memset(&main_elf, 0, sizeof(main_elf));
    ret = load_user_elf_image(buf, file_size, group, group->name, USER_DYN_MAIN_BASE, &main_elf);
    loaded_user_elf_t interp_elf;
    memset(&interp_elf, 0, sizeof(interp_elf));
    if (ret == 0 && interp_buf != NULL)
    {
        ret = load_user_elf_image(interp_buf, interp_size, group, interp_path, USER_INTERP_BASE, &interp_elf);
    }

    switch_page_directory(current_pagedir); // 恢复页表
    restore_runtime_state(was_scheduler_enabled, is_sti);

    if (ret < 0)
    {
        free_file_image(interp_buf, interp_size);
        free_file_image(buf, file_size);
        write_serial_fmt("Parse ELF file failed. Reason: Cannot map ELF %s, ret=%d\n",
                         interp_buf != NULL ? interp_path : path, ret);
        return (uint64_t)ret;
    }

    group->linux_abi = interp_buf != NULL || ehdr.e_ident[EI_OSABI] == ELFOSABI_LINUX;
    group->load_start = main_elf.map_start;
    group->aux_phdr = main_elf.phdr;
    group->aux_phent = main_elf.phent;
    group->aux_phnum = main_elf.phnum;
    group->aux_base = interp_buf != NULL ? interp_elf.load_bias : 0;
    group->aux_entry = main_elf.entry;
    group->aux_execfn = 0;
    group->elf_file = NULL;
    group->elf_size = file_size;

    uint64_t entry = interp_buf != NULL ? interp_elf.entry : main_elf.entry;
    if (interp_buf != NULL)
    {
        write_serial_fmt("ELF interpreter: %s base=0x%llx entry=0x%llx main_entry=0x%llx\n",
                         interp_path, interp_elf.load_bias, interp_elf.entry, main_elf.entry);
    }

    free_file_image(interp_buf, interp_size);
    free_file_image(buf, file_size);

    // 设置程序入口点
    return entry;
}//超进化吧，宝可梦

void getFileDirectory(const char *path, char *result)
{
    // 查找最后一个路径分隔符的位置
    int lastSlash = -1;
    for (int i = 0; path[i] != '\0'; i++)
    {
        if (path[i] == '/' || path[i] == '\\') { lastSlash = i; }
    }

    // 根据不同的情况处理
    if (lastSlash == -1)
    {
        // 没有分隔符，返回空字符串
        result[0] = '\0';
    }
    else if (lastSlash == 0)
    {
        // 只有根目录，如 "/file"
        result[0] = '/';
        result[1] = '\0';
    }
    else
    {
        // 正常路径，复制目录部分
        for (int i = 0; i < lastSlash; i++)
        {
            result[i] = path[i];
        }
        result[lastSlash] = '\0';
    }
}

char *getCwd(char *path)
{
    char result[strlen(path) + 1];
    memset(result, 0, strlen(path) + 1);
    getFileDirectory(path, result);
    return strdup(result);
}

static char **copy_process_argv(char *path, char *argv[], int argc)
{
    if (argc <= 0) return NULL;
    char **copy = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (copy == NULL) return NULL;

    for (int i = 0; i < argc; i++)
    {
        const char *src = argv != NULL ? argv[i] : (i == 0 ? path : NULL);
        if (src == NULL)
        {
            for (int j = 0; j < i; j++)
                free(copy[j]);
            free(copy);
            return NULL;
        }
        copy[i] = strdup(src);
        if (copy[i] == NULL)
        {
            for (int j = 0; j < i; j++)
                free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    copy[argc] = NULL;
    return copy;
}

static void free_process_argv(char **argv)
{
    if (argv == NULL) return;
    for (int i = 0; argv[i] != NULL; i++)
        free(argv[i]);
    free(argv);
}

int create_user_process_from_file(char *path, pcb_t pcb, char *argv[])
{
    if (path == NULL)
    {
        write_serial_fmt("Create User Process Failed. Reason: Empty Path.\n");
        return -EINVAL;
    }

    char  process_name[32];
    void *entry;
    int   argc = 0;

    get_thread_name_from_filepath(path, process_name);

    write_serial_fmt("Creating User Process. Process Name: %s\n", process_name);
    page_directory_t *new_directory = clone_page_directory(get_kernel_pagedir(), false);
    if (new_directory == NULL)
    {
        write_serial_fmt("Create User Process Failed. Reason: Cannot Clone Page Directory.\n");
        return -ENOMEM;
    }
    pcb_t             group         = create_process_group(process_name, pcb, new_directory, (char *)"");
    if (group == NULL)
    {
        free_page_directory(new_directory);
        write_serial_fmt("Create User Process Failed. Reason: Cannot Create Process Group.\n");
        return -ENOMEM;
    }
    group->exe_path = strdup(path);
    if (group->exe_path == NULL)
    {
        write_serial_fmt("Create User Process Failed. Reason: Cannot Copy Exec Path.\n");
        kill_proc(group, 0, false);
        return -ENOMEM;
    }

    entry = (void *)parse_elf_file(path, group);

    if (entry == NULL)
    {
        write_serial_fmt("Create User Process Failed. Reason: Cannot Open File.\n");
        kill_proc(group, 0, false);
        return -ENOENT;
    }
    if ((int64_t)entry < 0)
    {
        write_serial_fmt("Create User Process Failed. Reason: Exec Format Error.\n");
        int ret = (int)(int64_t)entry;
        kill_proc(group, 0, false);
        return ret;
    }

    write_serial_fmt("Creating User Thread. Thread Name: %s\n", process_name);
    char cmdline[PAGE_SIZE];
    memset(cmdline, 0, sizeof(cmdline));
    char *cmdline_ptr = cmdline;

    if (argv != NULL)
    {
        for (int i = 0; argv[i]; i++)
        {
            write_serial_fmt("[busybox-debug] create_process argv[%d]=%s\n", i, argv[i]);
            if (!append_cmdline_arg(cmdline, sizeof(cmdline), &cmdline_ptr, argv[i]))
            {
                write_serial_fmt("Create User Process Failed. Reason: Command Line Too Long.\n");
                kill_proc(group, 0, false);
                return -E2BIG;
            }
            argc++;
        }
    }
    else if (group->linux_abi)
    {
        if (!append_cmdline_arg(cmdline, sizeof(cmdline), &cmdline_ptr, path))
        {
            write_serial_fmt("Create User Process Failed. Reason: Command Line Too Long.\n");
            kill_proc(group, 0, false);
            return -E2BIG;
        }
        argc++;
    }
    char **thread_argv = copy_process_argv(path, argv, argc);
    if (argc > 0 && thread_argv == NULL)
    {
        write_serial_fmt("Create User Process Failed. Reason: Cannot Copy Argv.\n");
        kill_proc(group, 0, false);
        return -ENOMEM;
    }

    char *old_cmdline = group->cmdline;
    group->cmdline    = strdup(cmdline);
    write_serial_fmt("[busybox-debug] create_process cmdline=%s argc=%d linux_abi=%d\n",
                     cmdline,
                     argc,
                     group->linux_abi);
    if (group->cmdline == NULL)
    {
        group->cmdline = old_cmdline;
        free_process_argv(thread_argv);
        kill_proc(group, 0, false);
        return -ENOMEM;
    }
    if (old_cmdline != NULL) free(old_cmdline);
    group->argv = copy_process_argv(path, argv, argc);
    if (argc > 0 && group->argv == NULL)
    {
        free_process_argv(thread_argv);
        kill_proc(group, 0, false);
        return -ENOMEM;
    }
    group->argc = argc;
    char *cwd = getCwd(path);
    if (cwd == NULL)
    {
        free_process_argv(thread_argv);
        kill_proc(group, 0, false);
        return -ENOMEM;
    }
    write_serial_fmt("CWD: %s\n", cwd);
    size_t tid = create_user_thread(entry, thread_argv, argc, process_name, group, cwd);
    if ((int64_t)tid < 0)
    {
        kill_proc(group, 0, false);
        return (int)tid;
    }

    return group->pid;
}

int create_user_process_singleton_from_file(char *path, pcb_t pcb, char *argv[])
{
    pcb_t existing = found_process_by_exe_path(path);
    if (existing != NULL)
    {
        return (int)existing->pid;
    }

    return create_user_process_from_file(path, pcb, argv);
}

void create_user_thread_from_file(char *path, pcb_t pcb)
{
    char  thread_name[50];
    void *entry = (void *)parse_elf_file(path, pcb);
    if (entry == NULL || (int64_t)entry < 0) return;
    get_thread_name_from_filepath(path, thread_name);
    write_serial_fmt("Creating User Thread. Thread Name: %s\n", thread_name);
    char *cwd = getCwd(path);
    if (cwd == NULL) return;
    size_t tid = create_user_thread(entry, NULL, NULL, thread_name, pcb, cwd);
    (void)tid;
}
