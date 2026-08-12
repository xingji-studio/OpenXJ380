#pragma once

#ifdef XJ380_CANDIDATE_HOST_TEST
#include <stddef.h>
#include <stdint.h>
typedef struct page_directory { void *table; } page_directory_t;
typedef struct lock_queue { void *opaque; } lock_queue;
typedef struct vma_manager {
    void *vma_list;
    unsigned long vm_total;
    unsigned long vm_used;
} vma_manager_t;
#else
#include <krlibc.h>
#include <lock_queue.h>
#include <mm/page.h>
#include <mm/vma.h>
#include <stdint.h>
#endif

typedef enum user_image_candidate_state {
    USER_IMAGE_EMPTY = 0,
    USER_IMAGE_PREPARING,
    USER_IMAGE_PREPARED,
    USER_IMAGE_COMMITTED,
    USER_IMAGE_ABORTED,
} user_image_candidate_state_t;

typedef enum user_image_buffer_release {
    USER_IMAGE_BUFFER_RELEASE_HEAP = 0,
    USER_IMAGE_BUFFER_RELEASE_FRAMES,
} user_image_buffer_release_t;

typedef struct user_image_buffer {
    void *data;
    size_t size;
    user_image_buffer_release_t release;
} user_image_buffer_t;

/* Startup inputs stay owned by the candidate until its image is committed. */
typedef struct user_image_startup_storage {
    char *cmdline;
    char **argv;
    size_t argc;
    char **envp;
    size_t envc;
    uint64_t user_stack;
    uint64_t user_stack_top;
    uint64_t initial_rsp;
    uint64_t initial_argv;
    uint64_t initial_envp;
    uint64_t entry_rdx;
} user_image_startup_storage_t;

typedef struct user_image_address_space_owner {
    page_directory_t *pagedir;
    vma_manager_t *vma_manager;
    lock_queue *virt_queue;
} user_image_address_space_owner_t;

/* This is loader-internal state. It must not be derived from a live PCB. */
typedef struct user_image_candidate_context {
    user_image_candidate_state_t state;
    page_directory_t *pagedir;
    vma_manager_t vma_manager;
    lock_queue *virt_queue;
    void *startup_storage;
    user_image_buffer_t main_elf;
    char *exe_path;
    char *cmdline;
    char **argv;
    size_t argc;
    char **envp;
    size_t envc;
    char process_name[32];
    uint64_t entry;
    uint64_t aux_phdr;
    uint64_t aux_phent;
    uint64_t aux_phnum;
    uint64_t aux_base;
    uint64_t aux_entry;
    uint64_t aux_execfn;
    uint64_t user_stack;
    uint64_t user_stack_top;
    uint64_t initial_rsp;
    uint64_t initial_argv;
    uint64_t initial_envp;
    uint64_t entry_rdx;
} user_image_candidate_context_t;

typedef struct user_image_process_state {
    page_directory_t *pagedir;
    vma_manager_t vma_manager;
    lock_queue *virt_queue;
    void *startup_storage;
    user_image_buffer_t main_elf;
    char *exe_path;
    char *cmdline;
    char **argv;
    size_t argc;
    char **envp;
    size_t envc;
    uint64_t entry;
    uint64_t aux_phdr;
    uint64_t aux_phent;
    uint64_t aux_phnum;
    uint64_t aux_base;
    uint64_t aux_entry;
    uint64_t aux_execfn;
    uint64_t user_stack;
    uint64_t user_stack_top;
    uint64_t initial_rsp;
    uint64_t initial_argv;
    uint64_t initial_envp;
    uint64_t entry_rdx;
} user_image_process_state_t;

typedef struct user_image_snapshot {
    bool active;
    bool retired;
    bool quiescent;
    user_image_process_state_t image;
} user_image_snapshot_t;

/* Call mark_quiescent only after every CPU can no longer use queued page directories. */
typedef struct user_image_retirement_queue {
    void *head;
    void *tail;
    size_t pending;
} user_image_retirement_queue_t;

void user_image_candidate_init(user_image_candidate_context_t *candidate);
void user_image_candidate_begin(user_image_candidate_context_t *candidate);
bool user_image_candidate_mark_prepared(user_image_candidate_context_t *candidate);
bool user_image_candidate_address_space_owner(const user_image_candidate_context_t *candidate,
                                              user_image_address_space_owner_t *owner);
/* Maps ELF images into an already-owned candidate address space. It never commits a PCB. */
int user_image_prepare_elf(user_image_candidate_context_t *candidate, const char *path);
int user_image_prepare_startup(user_image_candidate_context_t *candidate, const char *path, const char *process_name,
                               char *argv[], char *envp[], size_t envc);
int user_image_prepare_candidate(user_image_candidate_context_t *candidate, const char *path, const char *process_name,
                                 char *argv[], char *envp[], size_t envc, const void *user);
void user_image_candidate_discard_elf_buffers(user_image_candidate_context_t *candidate);
void user_image_snapshot_init(user_image_snapshot_t *snapshot);
bool user_image_commit_locked(user_image_process_state_t *process,
                              user_image_candidate_context_t *candidate,
                              user_image_snapshot_t *old_image);
void user_image_abort(user_image_candidate_context_t *candidate);
void user_image_retire_old(user_image_snapshot_t *old_image);
void user_image_retirement_queue_init(user_image_retirement_queue_t *queue);
bool user_image_retirement_enqueue(user_image_retirement_queue_t *queue, user_image_snapshot_t *old_image);
void user_image_retirement_mark_quiescent(user_image_retirement_queue_t *queue);
size_t user_image_retirement_drain(user_image_retirement_queue_t *queue);
