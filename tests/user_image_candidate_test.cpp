#include <user_image_candidate.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t freed_directories;

extern "C" void free_page_directory(page_directory_t *directory)
{
    freed_directories++;
    free(directory);
}

extern "C" void vma_manager_exit_cleanup(vma_manager_t *manager)
{
    memset(manager, 0, sizeof(*manager));
}

extern "C" void user_image_candidate_free_frames(void *buffer, size_t size)
{
    (void)size;
    free(buffer);
}

static char *copy_string(const char *text)
{
    char *copy = strdup(text);
    assert(copy != NULL);
    return copy;
}

int main()
{
    user_image_candidate_context_t candidate;
    user_image_candidate_init(&candidate);
    user_image_abort(&candidate);
    user_image_abort(&candidate);
    assert(candidate.state == USER_IMAGE_ABORTED);

    user_image_candidate_init(&candidate);
    user_image_candidate_begin(&candidate);
    candidate.pagedir = (page_directory_t *)calloc(1, sizeof(page_directory_t));
    candidate.virt_queue = (lock_queue *)calloc(1, sizeof(lock_queue));
    candidate.main_elf.data = malloc(8);
    candidate.main_elf.size = 8;
    candidate.cmdline = copy_string("new");
    candidate.exe_path = copy_string("/new");
    user_image_address_space_owner_t owner = {};
    assert(user_image_candidate_address_space_owner(&candidate, &owner));
    assert(owner.pagedir == candidate.pagedir && owner.vma_manager == &candidate.vma_manager &&
           owner.virt_queue == candidate.virt_queue);

    user_image_process_state_t process = {};
    process.cmdline = copy_string("old");
    process.exe_path = copy_string("/old");
    user_image_snapshot_t old_image;
    user_image_snapshot_init(&old_image);

    assert(!user_image_commit_locked(&process, &candidate, &old_image));
    assert(strcmp(process.cmdline, "old") == 0);
    assert(user_image_candidate_mark_prepared(&candidate));
    assert(user_image_commit_locked(&process, &candidate, &old_image));
    assert(candidate.state == USER_IMAGE_COMMITTED);
    assert(strcmp(process.cmdline, "new") == 0 && strcmp(old_image.image.cmdline, "old") == 0);

    user_image_retirement_queue_t retirement_queue;
    user_image_retirement_queue_init(&retirement_queue);
    old_image.image.pagedir = (page_directory_t *)calloc(1, sizeof(page_directory_t));
    assert(old_image.image.pagedir != NULL);
    assert(user_image_retirement_enqueue(&retirement_queue, &old_image));
    assert(!old_image.active && retirement_queue.pending == 1);
    assert(user_image_retirement_drain(&retirement_queue) == 0 && freed_directories == 0);
    user_image_retirement_mark_quiescent(&retirement_queue);
    assert(user_image_retirement_drain(&retirement_queue) == 1 && freed_directories == 1);

    user_image_snapshot_t queued_image;
    user_image_snapshot_init(&queued_image);
    queued_image.active = true;
    queued_image.image.pagedir = (page_directory_t *)calloc(1, sizeof(page_directory_t));
    assert(queued_image.image.pagedir != NULL);
    assert(user_image_retirement_enqueue(&retirement_queue, &queued_image));
    assert(!queued_image.active && retirement_queue.pending == 1);
    assert(user_image_retirement_drain(&retirement_queue) == 0 && freed_directories == 1);
    user_image_retirement_mark_quiescent(&retirement_queue);
    assert(user_image_retirement_drain(&retirement_queue) == 1 && retirement_queue.pending == 0);
    assert(freed_directories == 2);

    user_image_candidate_context_t aborted;
    user_image_candidate_init(&aborted);
    user_image_candidate_begin(&aborted);
    user_image_abort(&aborted);
    assert(aborted.state == USER_IMAGE_ABORTED);

    free(process.cmdline);
    free(process.exe_path);
    return 0;
}
