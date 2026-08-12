#include <user_image_candidate.h>
#include <errno.h>

#ifdef XJ380_CANDIDATE_HOST_TEST
#include <stdlib.h>
#include <string.h>
extern "C" void free_page_directory(page_directory_t *directory);
extern "C" void vma_manager_exit_cleanup(vma_manager_t *manager);
extern "C" void user_image_candidate_free_frames(void *buffer, size_t size);
#else
#include <mm/hhdm.h>
#include <proto.hpp>

static spin_t user_image_retirement_lock = SPIN_INIT;

class user_image_retirement_guard
{
public:
    user_image_retirement_guard()
    {
        spin_lock(&user_image_retirement_lock);
    }

    ~user_image_retirement_guard()
    {
        spin_unlock(&user_image_retirement_lock);
    }
};
#endif

typedef struct user_image_retirement_node {
    user_image_snapshot_t snapshot;
    struct user_image_retirement_node *next;
} user_image_retirement_node_t;

static void clear_buffer(user_image_buffer_t *buffer)
{
    if (buffer == NULL || buffer->data == NULL) return;
    if (buffer->release == USER_IMAGE_BUFFER_RELEASE_FRAMES)
    {
#ifdef XJ380_CANDIDATE_HOST_TEST
        user_image_candidate_free_frames(buffer->data, buffer->size);
#else
        free_frames((uint64_t)virt_to_phys((uint64_t)buffer->data),
                    (buffer->size + PAGE_SIZE - 1) / PAGE_SIZE);
#endif
    }
    else
        free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void clear_vector(char ***vector)
{
    if (vector == NULL || *vector == NULL) return;
    for (char **entry = *vector; *entry != NULL; ++entry) free(*entry);
    free(*vector);
    *vector = NULL;
}

static char **copy_vector(char *vector[], size_t count)
{
    if (count == 0) return NULL;
    char **copy = (char **)calloc(count + 1, sizeof(char *));
    if (copy == NULL) return NULL;
    for (size_t i = 0; i < count; ++i)
    {
        if (vector == NULL || vector[i] == NULL)
        {
            clear_vector(&copy);
            return NULL;
        }
        copy[i] = strdup(vector[i]);
        if (copy[i] == NULL)
        {
            clear_vector(&copy);
            return NULL;
        }
    }
    return copy;
}

static void clear_process_image(user_image_process_state_t *image)
{
    if (image == NULL) return;
    free(image->startup_storage);
    clear_buffer(&image->main_elf);
    clear_vector(&image->argv);
    clear_vector(&image->envp);
    free(image->exe_path);
    free(image->cmdline);
    if (image->virt_queue != NULL)
    {
#ifdef XJ380_CANDIDATE_HOST_TEST
        free(image->virt_queue);
#else
        while (void *data = queue_dequeue(image->virt_queue)) free(data);
        queue_destroy(image->virt_queue);
#endif
    }
    vma_manager_exit_cleanup(&image->vma_manager);
    if (image->pagedir != NULL) free_page_directory(image->pagedir);
    memset(image, 0, sizeof(*image));
}

static void move_candidate(user_image_process_state_t *destination, user_image_candidate_context_t *source)
{
    destination->pagedir = source->pagedir;
    destination->vma_manager = source->vma_manager;
    destination->virt_queue = source->virt_queue;
    destination->startup_storage = source->startup_storage;
    destination->main_elf = source->main_elf;
    destination->exe_path = source->exe_path;
    destination->cmdline = source->cmdline;
    destination->argv = source->argv;
    destination->argc = source->argc;
    destination->envp = source->envp;
    destination->envc = source->envc;
    destination->entry = source->entry;
    destination->aux_phdr = source->aux_phdr;
    destination->aux_phent = source->aux_phent;
    destination->aux_phnum = source->aux_phnum;
    destination->aux_base = source->aux_base;
    destination->aux_entry = source->aux_entry;
    destination->aux_execfn = source->aux_execfn;
    destination->user_stack = source->user_stack;
    destination->user_stack_top = source->user_stack_top;
    destination->initial_rsp = source->initial_rsp;
    destination->initial_argv = source->initial_argv;
    destination->initial_envp = source->initial_envp;
    destination->entry_rdx = source->entry_rdx;
}

void user_image_candidate_init(user_image_candidate_context_t *candidate)
{
    if (candidate == NULL) return;
    memset(candidate, 0, sizeof(*candidate));
    candidate->state = USER_IMAGE_EMPTY;
}

void user_image_candidate_begin(user_image_candidate_context_t *candidate)
{
    if (candidate != NULL && candidate->state == USER_IMAGE_EMPTY) candidate->state = USER_IMAGE_PREPARING;
}

bool user_image_candidate_mark_prepared(user_image_candidate_context_t *candidate)
{
    if (candidate == NULL || candidate->state != USER_IMAGE_PREPARING) return false;
    candidate->state = USER_IMAGE_PREPARED;
    return true;
}

bool user_image_candidate_address_space_owner(const user_image_candidate_context_t *candidate,
                                              user_image_address_space_owner_t *owner)
{
    if (candidate == NULL || owner == NULL || candidate->state != USER_IMAGE_PREPARING ||
        candidate->pagedir == NULL || candidate->virt_queue == NULL)
        return false;
    owner->pagedir = candidate->pagedir;
    owner->vma_manager = const_cast<vma_manager_t *>(&candidate->vma_manager);
    owner->virt_queue = candidate->virt_queue;
    return true;
}

void user_image_snapshot_init(user_image_snapshot_t *snapshot)
{
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
}

int user_image_prepare_startup(user_image_candidate_context_t *candidate, const char *path, const char *process_name,
                               char *argv[], char *envp[], size_t envc)
{
    if (candidate == NULL || path == NULL || candidate->state != USER_IMAGE_PREPARING ||
        candidate->startup_storage != NULL)
        return -EINVAL;

    static constexpr size_t startup_vector_limit = 256;
    static constexpr size_t startup_cmdline_limit = 4096;
    size_t argc = 0;
    if (argv != NULL)
        while (argv[argc] != NULL)
        {
            if (argc == startup_vector_limit) return -E2BIG;
            ++argc;
        }
    else
        argc = 1;
    size_t actual_envc = 0;
    if (envp != NULL)
        while (actual_envc < envc && envp[actual_envc] != NULL)
            ++actual_envc;

    user_image_startup_storage_t *startup = (user_image_startup_storage_t *)calloc(1, sizeof(*startup));
    if (startup == NULL) return -ENOMEM;
    char *default_argv[] = {(char *)path};
    startup->argv = argv != NULL ? copy_vector(argv, argc) : copy_vector(default_argv, argc);
    startup->envp = copy_vector(envp, actual_envc);
    if ((argc != 0 && startup->argv == NULL) || (actual_envc != 0 && startup->envp == NULL))
    {
        clear_vector(&startup->argv);
        clear_vector(&startup->envp);
        free(startup);
        return -ENOMEM;
    }

    size_t cmdline_size = 1;
    for (size_t i = 0; i < argc; ++i)
    {
        size_t argument_size = strlen(startup->argv[i]) + (i != 0 ? 1 : 0);
        if (argument_size < strlen(startup->argv[i]) || cmdline_size > startup_cmdline_limit ||
            argument_size > startup_cmdline_limit - cmdline_size)
        {
            clear_vector(&startup->argv);
            clear_vector(&startup->envp);
            free(startup);
            return -E2BIG;
        }
        cmdline_size += strlen(startup->argv[i]) + (i != 0 ? 1 : 0);
    }
    startup->cmdline = (char *)malloc(cmdline_size);
    if (startup->cmdline == NULL)
    {
        clear_vector(&startup->argv);
        clear_vector(&startup->envp);
        free(startup);
        return -ENOMEM;
    }

    char *cursor = startup->cmdline;
    for (size_t i = 0; i < argc; ++i)
    {
        if (i != 0) *(cursor++) = ' ';
        size_t length = strlen(startup->argv[i]);
        memcpy(cursor, startup->argv[i], length);
        cursor += length;
    }
    *cursor = '\0';

    startup->argc = argc;
    startup->envc = actual_envc;
    candidate->startup_storage = startup;
    candidate->argv = startup->argv;
    candidate->argc = startup->argc;
    candidate->envp = startup->envp;
    candidate->envc = startup->envc;
    candidate->cmdline = startup->cmdline;
    strncpy(candidate->process_name, process_name != NULL ? process_name : "", sizeof(candidate->process_name) - 1);
    return 0;
}

void user_image_candidate_discard_elf_buffers(user_image_candidate_context_t *candidate)
{
    if (candidate == NULL || (candidate->state != USER_IMAGE_PREPARING && candidate->state != USER_IMAGE_PREPARED))
        return;
    clear_buffer(&candidate->main_elf);
}

void user_image_abort(user_image_candidate_context_t *candidate)
{
    if (candidate == NULL || candidate->state == USER_IMAGE_ABORTED || candidate->state == USER_IMAGE_COMMITTED) return;
    user_image_process_state_t image = {};
    move_candidate(&image, candidate);
    clear_process_image(&image);
    memset(candidate, 0, sizeof(*candidate));
    candidate->state = USER_IMAGE_ABORTED;
}

bool user_image_commit_locked(user_image_process_state_t *process,
                              user_image_candidate_context_t *candidate,
                              user_image_snapshot_t *old_image)
{
    if (process == NULL || candidate == NULL || old_image == NULL || candidate->state != USER_IMAGE_PREPARED ||
        old_image->active || old_image->retired)
        return false;
    old_image->active = true;
    old_image->image = *process;
    memset(process, 0, sizeof(*process));
    move_candidate(process, candidate);
    memset(candidate, 0, sizeof(*candidate));
    candidate->state = USER_IMAGE_COMMITTED;
    return true;
}

void user_image_retire_old(user_image_snapshot_t *old_image)
{
    if (old_image == NULL || !old_image->active || old_image->retired ||
        (old_image->image.pagedir != NULL && !old_image->quiescent))
        return;
    clear_process_image(&old_image->image);
    old_image->active = false;
    old_image->retired = true;
}

void user_image_retirement_queue_init(user_image_retirement_queue_t *queue)
{
    if (queue != NULL) memset(queue, 0, sizeof(*queue));
}

bool user_image_retirement_enqueue(user_image_retirement_queue_t *queue, user_image_snapshot_t *old_image)
{
    if (queue == NULL || old_image == NULL || !old_image->active || old_image->retired) return false;
    user_image_retirement_node_t *node = (user_image_retirement_node_t *)calloc(1, sizeof(*node));
    if (node == NULL) return false;

    node->snapshot = *old_image;
    memset(old_image, 0, sizeof(*old_image));
#ifndef XJ380_CANDIDATE_HOST_TEST
    user_image_retirement_guard guard;
#endif
    user_image_retirement_node_t *tail = (user_image_retirement_node_t *)queue->tail;
    if (tail != NULL)
        tail->next = node;
    else
        queue->head = node;
    queue->tail = node;
    ++queue->pending;
    return true;
}

void user_image_retirement_mark_quiescent(user_image_retirement_queue_t *queue)
{
    if (queue == NULL) return;
#ifndef XJ380_CANDIDATE_HOST_TEST
    user_image_retirement_guard guard;
#endif
    for (user_image_retirement_node_t *node = (user_image_retirement_node_t *)queue->head; node != NULL;
         node = node->next)
        node->snapshot.quiescent = true;
}

size_t user_image_retirement_drain(user_image_retirement_queue_t *queue)
{
    if (queue == NULL) return 0;
    size_t retired = 0;
#ifndef XJ380_CANDIDATE_HOST_TEST
    user_image_retirement_guard guard;
#endif
    user_image_retirement_node_t **link = (user_image_retirement_node_t **)&queue->head;
    while (*link != NULL)
    {
        user_image_retirement_node_t *node = *link;
        if (!node->snapshot.quiescent)
        {
            link = &node->next;
            continue;
        }
        user_image_retire_old(&node->snapshot);
        if (!node->snapshot.retired)
        {
            link = &node->next;
            continue;
        }
        *link = node->next;
        free(node);
        --queue->pending;
        ++retired;
    }
    queue->tail = NULL;
    for (user_image_retirement_node_t *node = (user_image_retirement_node_t *)queue->head; node != NULL;
         node = node->next)
        queue->tail = node;
    return retired;
}
