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
#include <mm/uaccess.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <rtc.h>
#include <stdint.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/user.h>
#include <user_image_candidate.h>
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

static int load_user_elf_image(uint8_t *buf, uint64_t file_size, user_image_address_space_owner_t *owner,
                               const char *name, uint64_t dyn_base, loaded_user_elf_t *out)
{
    if (buf == NULL || owner == NULL || owner->pagedir == NULL || owner->vma_manager == NULL || out == NULL)
        return -EINVAL;

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

    if (!page_map_range_to_random_checked(owner->pagedir, load_start, load_end - load_start,
                                          PTE_PRESENT | PTE_WRITEABLE | PTE_USER))
        return -ENOMEM;
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
        vma_insert(owner->vma_manager, elf_vma);
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

static int read_candidate_elf(const char *path, user_image_buffer_t *buffer)
{
    if (path == NULL || buffer == NULL || buffer->data != NULL) return -EINVAL;

    vfs_node_t file = vfs_open(path);
    if (file == NULL) return -ENOENT;

    uint8_t *data = NULL;
    uint64_t size = 0;
    int result = read_vfs_file(file, &data, &size);
    vfs_close(file);
    if (result < 0) return result;

    buffer->data = data;
    buffer->size = size;
    buffer->release = USER_IMAGE_BUFFER_RELEASE_FRAMES;
    return 0;
}

int user_image_prepare_elf(user_image_candidate_context_t *candidate, const char *path)
{
    user_image_address_space_owner_t owner = {};
    if (candidate == NULL || path == NULL || candidate->state != USER_IMAGE_PREPARING ||
        candidate->main_elf.data != NULL || !user_image_candidate_address_space_owner(candidate, &owner))
        return -EINVAL;

    int result = read_candidate_elf(path, &candidate->main_elf);
    if (result < 0) return result;
    if (candidate->main_elf.size < sizeof(Elf64_Ehdr)) return -ENOEXEC;

    bool interrupts_enabled = are_interrupts_enabled();
    bool scheduler_enabled = is_scheduler;
    if (!no_interrupt) close_interrupt;

    page_directory_t *previous_directory = get_current_directory();
    switch_page_directory(candidate->pagedir);

    loaded_user_elf_t main_image = {};
    result = load_user_elf_image((uint8_t *)candidate->main_elf.data, candidate->main_elf.size, &owner,
                                 path, USER_DYN_MAIN_BASE, &main_image);
    switch_page_directory(previous_directory);
    restore_runtime_state(scheduler_enabled, interrupts_enabled);
    if (result < 0) return result;

    candidate->exe_path = strdup(path);
    if (candidate->exe_path == NULL) return -ENOMEM;
    candidate->entry = main_image.entry;
    candidate->aux_phdr = main_image.phdr;
    candidate->aux_phent = main_image.phent;
    candidate->aux_phnum = main_image.phnum;
    candidate->aux_base = 0;
    candidate->aux_entry = main_image.entry;
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

    bool is_sti                = are_interrupts_enabled();
    bool was_scheduler_enabled = is_scheduler;
    if (!no_interrupt) close_interrupt;
    disable_scheduler();

    page_directory_t *current_pagedir = get_current_directory();
    switch_page_directory(group->pagedir);

    loaded_user_elf_t main_elf;
    memset(&main_elf, 0, sizeof(main_elf));
    user_image_address_space_owner_t owner = {group->pagedir, &group->vma_manager, group->virt_queue};
    ret = load_user_elf_image(buf, file_size, &owner, group->name, USER_DYN_MAIN_BASE, &main_elf);
    switch_page_directory(current_pagedir); // 恢复页表
    restore_runtime_state(was_scheduler_enabled, is_sti);

    if (ret < 0)
    {
        free_file_image(buf, file_size);
        write_serial_fmt("Parse ELF file failed. Reason: Cannot map ELF %s, ret=%d\n", path, ret);
        return (uint64_t)ret;
    }

    group->load_start = main_elf.map_start;
    group->aux_phdr = main_elf.phdr;
    group->aux_phent = main_elf.phent;
    group->aux_phnum = main_elf.phnum;
    group->aux_base = 0;
    group->aux_entry = main_elf.entry;
    group->aux_execfn = 0;
    group->elf_file = NULL;
    group->elf_size = file_size;

    free_file_image(buf, file_size);

    // 设置程序入口点
    return main_elf.entry;
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

static bool candidate_stack_push(user_image_candidate_context_t *candidate, uint64_t *stack, const void *data,
                                 size_t length)
{
    if (candidate == NULL || stack == NULL || data == NULL || length == 0 || *stack < candidate->user_stack + length)
        return false;
    *stack -= length;
    *stack &= ~0x7ULL;
    lazy_address_space_owner_t owner = {candidate->pagedir, candidate->virt_queue};
    uint64_t first_page = *stack & PAGE_MASK;
    uint64_t last_page = (*stack + length - 1) & PAGE_MASK;
    for (uint64_t page = first_page;; page += PAGE_SIZE)
    {
        if (translate_address(candidate->pagedir, page) == 0 && lazy_tryalloc_owner(&owner, page) != EOK) return false;
        if (page == last_page) break;
    }
    return copy_to_user_pagedir(candidate->pagedir, (void *)*stack, data, length);
}

static int prepare_candidate_initial_stack(user_image_candidate_context_t *candidate, const UserInfo *user)
{
    if (candidate == NULL || candidate->state != USER_IMAGE_PREPARING || candidate->startup_storage == NULL)
        return -EINVAL;

    user_image_startup_storage_t *startup = (user_image_startup_storage_t *)candidate->startup_storage;
    lazy_address_space_owner_t owner = {candidate->pagedir, candidate->virt_queue};
    uint64_t stack_base = page_reserve_user_range_owner(&owner, BIG_USER_STACK);
    if (stack_base == 0) return -ENOMEM;
    candidate->user_stack = stack_base;
    candidate->user_stack_top = stack_base + BIG_USER_STACK;

    uint64_t stack = candidate->user_stack_top;
    static constexpr size_t candidate_vector_limit = 256;
    uint64_t argv_ptrs[candidate_vector_limit + 1] = {};
    uint64_t envp_ptrs[candidate_vector_limit + 1] = {};
    if (startup->argc > candidate_vector_limit || startup->envc > candidate_vector_limit) return -E2BIG;

    const char platform[] = "x86_64";
    if (!candidate_stack_push(candidate, &stack, candidate->process_name, strlen(candidate->process_name) + 1))
        return -EFAULT;
    candidate->aux_execfn = stack;
    if (!candidate_stack_push(candidate, &stack, platform, sizeof(platform))) return -EFAULT;
    uint64_t platform_ptr = stack;

    for (size_t i = 0; i < startup->envc; ++i)
    {
        if (!candidate_stack_push(candidate, &stack, startup->envp[i], strlen(startup->envp[i]) + 1)) return -EFAULT;
        envp_ptrs[i] = stack;
    }
    for (size_t i = 0; i < startup->argc; ++i)
    {
        if (!candidate_stack_push(candidate, &stack, startup->argv[i], strlen(startup->argv[i]) + 1)) return -EFAULT;
        argv_ptrs[i] = stack;
    }
    if (startup->argc != 0) candidate->aux_execfn = argv_ptrs[0];

    uint8_t random_bytes[16];
    uint64_t seed = nanoTime() ^ (uint64_t)(uintptr_t)candidate ^ stack;
    for (size_t i = 0; i < sizeof(random_bytes); ++i)
    {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        random_bytes[i] = (uint8_t)(seed >> ((i & 7U) * 8));
    }
    if (!candidate_stack_push(candidate, &stack, random_bytes, sizeof(random_bytes))) return -EFAULT;
    uint64_t random_ptr = stack;

    struct auxv_entry { uint64_t type; uint64_t value; } auxv[] = {
        {AT_NULL, 0}, {AT_SECURE, 0}, {AT_EXECFN, candidate->aux_execfn}, {AT_CLKTCK, 100},
        {AT_PLATFORM, platform_ptr}, {AT_HWCAP, 0}, {AT_EGID, user_gid(user)}, {AT_GID, user_gid(user)},
        {AT_EUID, user_uid(user)}, {AT_UID, user_uid(user)}, {AT_FLAGS, 0}, {AT_BASE, candidate->aux_base},
        {AT_RANDOM, random_ptr}, {AT_ENTRY, candidate->aux_entry ? candidate->aux_entry : candidate->entry},
        {AT_PHNUM, candidate->aux_phnum}, {AT_PHENT, candidate->aux_phent}, {AT_PHDR, candidate->aux_phdr},
        {AT_PAGESZ, PAGE_SIZE},
    };
    uint64_t total = sizeof(auxv) + (startup->envc + startup->argc + 3) * sizeof(uint64_t);
    stack -= (stack - total) & 0xFULL;
    for (size_t i = 0; i < sizeof(auxv) / sizeof(auxv[0]); ++i)
        if (!candidate_stack_push(candidate, &stack, &auxv[i], sizeof(auxv[i]))) return -EFAULT;

    uint64_t null_pointer = 0;
    if (!candidate_stack_push(candidate, &stack, &null_pointer, sizeof(null_pointer)) ||
        !candidate_stack_push(candidate, &stack, envp_ptrs, startup->envc * sizeof(uint64_t)))
        return -EFAULT;
    uint64_t envp_ptr = stack;
    if (!candidate_stack_push(candidate, &stack, &null_pointer, sizeof(null_pointer)) ||
        !candidate_stack_push(candidate, &stack, argv_ptrs, startup->argc * sizeof(uint64_t)))
        return -EFAULT;
    uint64_t argc = startup->argc;
    if (!candidate_stack_push(candidate, &stack, &argc, sizeof(argc))) return -EFAULT;

    startup->user_stack = candidate->user_stack = stack_base;
    startup->user_stack_top = candidate->user_stack_top = stack_base + BIG_USER_STACK;
    startup->initial_rsp = candidate->initial_rsp = stack;
    startup->initial_argv = candidate->initial_argv = stack + sizeof(uint64_t);
    startup->initial_envp = candidate->initial_envp = envp_ptr;
    startup->entry_rdx = candidate->entry_rdx = envp_ptr;
    return 0;
}

int user_image_prepare_candidate(user_image_candidate_context_t *candidate, const char *path, const char *process_name,
                                 char *argv[], char *envp[], size_t envc, const void *user_data)
{
    const UserInfo *user = (const UserInfo *)user_data;
    if (candidate == NULL || path == NULL || process_name == NULL || user == NULL) return -EINVAL;

    user_image_candidate_init(candidate);
    user_image_candidate_begin(candidate);
    candidate->pagedir = clone_page_directory(get_kernel_pagedir(), false);
    candidate->virt_queue = queue_init();
    if (candidate->pagedir == NULL || candidate->virt_queue == NULL)
    {
        user_image_abort(candidate);
        return -ENOMEM;
    }

    int result = user_image_prepare_elf(candidate, path);
    if (result == 0) result = user_image_prepare_startup(candidate, path, process_name, argv, envp, envc);
    if (result == 0) result = prepare_candidate_initial_stack(candidate, user);
    if (result == 0 && !user_image_candidate_mark_prepared(candidate)) result = -EINVAL;
    if (result < 0) user_image_abort(candidate);
    return result;
}

int create_user_process_from_file(char *path, pcb_t pcb, char *argv[])
{
    if (path == NULL)
    {
        write_serial_fmt("Create User Process Failed. Reason: Empty Path.\n");
        return -EINVAL;
    }

    char process_name[32];

    get_thread_name_from_filepath(path, process_name);

    write_serial_fmt("Creating User Process. Process Name: %s\n", process_name);
    user_image_candidate_context_t candidate;
    UserInfo *user = current_user != NULL ? current_user : &root_user;
    char **startup_envp = pcb != NULL && pcb->envp != NULL ? pcb->envp : user->envp;
    size_t startup_envc = pcb != NULL && pcb->envp != NULL ? pcb->envc : user->envc;
    int result = user_image_prepare_candidate(&candidate, path, process_name, argv, startup_envp, startup_envc, user);
    if (result < 0)
    {
        write_serial_fmt("Create User Process Failed. Reason: Cannot Prepare Candidate.\n");
        return result;
    }

    char *cwd = getCwd(path);
    if (cwd == NULL)
    {
        user_image_abort(&candidate);
        return -ENOMEM;
    }

    /* The mapped images no longer need their file buffers after preparation. */
    user_image_candidate_discard_elf_buffers(&candidate);

    pcb_t group = create_process_group_unpublished(process_name, pcb, candidate.pagedir, (char *)"");
    if (group == NULL)
    {
        free(cwd);
        user_image_abort(&candidate);
        write_serial_fmt("Create User Process Failed. Reason: Cannot Create Process Group.\n");
        return -ENOMEM;
    }

    free(group->cmdline);
    free_process_argv(group->envp);
    queue_destroy(group->virt_queue);
    group->cmdline = NULL;
    group->envp = NULL;
    group->virt_queue = NULL;

    user_image_process_state_t image = {};
    user_image_snapshot_t discarded_image;
    user_image_snapshot_init(&discarded_image);
    if (!user_image_commit_locked(&image, &candidate, &discarded_image))
    {
        free(cwd);
        return -EINVAL;
    }
    user_image_retire_old(&discarded_image);

    group->pagedir = image.pagedir;
    group->vma_manager = image.vma_manager;
    group->virt_queue = image.virt_queue;
    group->exe_path = image.exe_path;
    group->cmdline = image.cmdline;
    group->argv = image.argv;
    group->argc = image.argc;
    group->envp = image.envp;
    group->envc = image.envc;
    group->aux_phdr = image.aux_phdr;
    group->aux_phent = image.aux_phent;
    group->aux_phnum = image.aux_phnum;
    group->aux_base = image.aux_base;
    group->aux_entry = image.aux_entry;
    group->aux_execfn = image.aux_execfn;
    group->prepared_user_stack = image.user_stack;
    group->prepared_user_stack_top = image.user_stack_top;
    group->prepared_user_rsp = image.initial_rsp;
    group->prepared_user_argv = image.initial_argv;
    group->prepared_user_envp = image.initial_envp;
    group->prepared_user_entry_rdx = image.entry_rdx;
    free(image.startup_storage);
    write_serial_fmt("Creating User Thread. Thread Name: %s\n", process_name);
    write_serial_fmt("CWD: %s\n", cwd);
    char **thread_argv = copy_process_argv(path, group->argv, (int)group->argc);
    if (group->argc != 0 && thread_argv == NULL)
    {
        free(cwd);
        kill_proc0(group);
        return -ENOMEM;
    }

    tcb_t new_task = NULL;
    size_t tid = create_user_thread_unpublished((void *)image.entry, thread_argv, group->argc, process_name, group,
                                                cwd, &new_task);
    if ((int64_t)tid < 0)
    {
        kill_proc0(group);
        return (int)tid;
    }
    if (!publish_process_group(group))
    {
        kill_proc0(group);
        return -ENOMEM;
    }
    if (!publish_user_thread(new_task))
    {
        kill_proc(group, 0, false);
        return -ENOMEM;
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
