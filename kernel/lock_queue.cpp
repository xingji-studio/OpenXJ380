#include "lock_queue.h"
#include "mm/heap.h"

static inline void queue_append_node(lock_queue *q, lock_node *new_node)
{
    new_node->next = NULL;
    if (q->head == NULL)
    {
        new_node->prev = NULL;
        q->head = new_node;
        q->tail = new_node;
    }
    else
    {
        new_node->prev = q->tail;
        q->tail->next = new_node;
        q->tail = new_node;
    }
    if (q->rr_cursor == NULL)
    {
        q->rr_cursor = q->head;
    }
    q->size++;
}

static inline void *queue_remove_locked(lock_queue *q, lock_node *node)
{
    if (q == NULL || node == NULL) return NULL;

    lock_node *next_cursor = node->next != NULL ? node->next : q->head;

    if (node->prev != NULL) node->prev->next = node->next;
    else q->head = node->next;

    if (node->next != NULL) node->next->prev = node->prev;
    else q->tail = node->prev;

    if (q->rr_cursor == node)
    {
        q->rr_cursor = (node->next != NULL) ? node->next : q->head;
    }

    if (q->head == NULL)
    {
        q->tail      = NULL;
        q->rr_cursor = NULL;
    }
    else if (q->rr_cursor == NULL)
    {
        q->rr_cursor = next_cursor != NULL ? next_cursor : q->head;
    }

    void *handle = node->data;
    free(node);
    q->size--;
    return handle;
}

lock_queue *queue_init()
{
    lock_queue *q = (lock_queue *)malloc(sizeof(lock_queue));
    if (q == NULL)
    {
        return NULL; // Allocation failed
    }
    memset(q, 0, sizeof(lock_queue));
    q->head = q->tail = NULL;
    q->rr_cursor      = NULL;
    q->next_index     = 0;
    q->size           = 0;
    q->lock           = SPIN_INIT;
    q->iter_lock      = SPIN_INIT;
    return q;
}

size_t lock_queue_enqueue(lock_queue *q, void *data)
{
    if (q == NULL) return (size_t)-1;
    lock_node *new_node = (lock_node *)malloc(sizeof(lock_node));
    if (!new_node) return (size_t)-1;

    new_node->data  = data;
    new_node->next  = NULL;
    new_node->prev  = NULL;

    spin_lock(&q->lock);
    if (q->next_index == (size_t)-1)
    {
        spin_unlock(&q->lock);
        free(new_node);
        return (size_t)-1;
    }
    new_node->index = q->next_index++;
    queue_append_node(q, new_node);
    spin_unlock(&q->lock);

    return new_node->index;
}

size_t queue_enqueue(lock_queue *q, void *data)
{
    if (q == NULL) return (size_t)-1;
    lock_node *new_node = (lock_node *)malloc(sizeof(lock_node));
    if (!new_node) return (size_t)-1;

    new_node->data  = data;
    new_node->next  = NULL;
    new_node->prev  = NULL;

    spin_lock(&q->lock);
    if (q->next_index == (size_t)-1)
    {
        spin_unlock(&q->lock);
        free(new_node);
        return (size_t)-1;
    }
    new_node->index = q->next_index++;
    queue_append_node(q, new_node);
    spin_unlock(&q->lock);

    return new_node->index;
}

/* Caller must already hold q->lock. This avoids re-entrant spinlock acquires
 * inside the lazy allocator, which used to deadlock the boot path. */
size_t queue_enqueue_locked(lock_queue *q, void *data)
{
    if (q == NULL) return (size_t)-1;
    lock_node *new_node = (lock_node *)malloc(sizeof(lock_node));
    if (!new_node) return (size_t)-1;

    new_node->data = data;
    new_node->next = NULL;
    new_node->prev = NULL;

    if (q->next_index == (size_t)-1)
    {
        free(new_node);
        return (size_t)-1;
    }
    new_node->index = q->next_index++;
    queue_append_node(q, new_node);
    return new_node->index;
}

size_t queue_enqueue_lowest(lock_queue *q, void *data)
{
    if (q == NULL) return (size_t)-1;
    lock_node *new_node = (lock_node *)malloc(sizeof(lock_node));
    if (!new_node) return (size_t)-1;

    new_node->data = data;
    new_node->next = NULL;
    new_node->prev = NULL;

    spin_lock(&q->lock);
    size_t id = 0;
    while (true)
    {
        bool used = false;
        for (lock_node *current = q->head; current != NULL; current = current->next)
        {
            if (current->index == id)
            {
                used = true;
                break;
            }
        }
        if (!used) break;
        id++;
    }
    if (id == (size_t)-1)
    {
        spin_unlock(&q->lock);
        free(new_node);
        return (size_t)-1;
    }

    new_node->index = id;
    queue_append_node(q, new_node);
    if (id >= q->next_index && id != (size_t)-1) q->next_index = id + 1;
    spin_unlock(&q->lock);

    return new_node->index;
}

size_t queue_enqueue_ref(lock_queue *q, void *data, lock_node **out_node)
{
    if (out_node != NULL) *out_node = NULL;
    if (q == NULL) return (size_t)-1;

    lock_node *new_node = (lock_node *)malloc(sizeof(lock_node));
    if (!new_node) return (size_t)-1;

    new_node->data = data;
    new_node->next = NULL;
    new_node->prev = NULL;

    spin_lock(&q->lock);
    if (q->next_index == (size_t)-1)
    {
        spin_unlock(&q->lock);
        free(new_node);
        return (size_t)-1;
    }
    new_node->index = q->next_index++;
    queue_append_node(q, new_node);
    spin_unlock(&q->lock);

    if (out_node != NULL) *out_node = new_node;
    return new_node->index;
}

size_t queue_enqueue_id(lock_queue *q, void *data, size_t id)
{
    if (q == NULL) return (size_t)-1;
    if (id == (size_t)-1) return (size_t)-1;
    lock_node *new_node = (lock_node *)malloc(sizeof(lock_node));
    if (!new_node) return (size_t)-1;

    new_node->data  = data;
    new_node->next  = NULL;
    new_node->prev  = NULL;

    spin_lock(&q->lock);
    for (lock_node *current = q->head; current != NULL; current = current->next)
    {
        if (current->index == id)
        {
            spin_unlock(&q->lock);
            free(new_node);
            return (size_t)-1;
        }
    }
    new_node->index = id;
    queue_append_node(q, new_node);
    if (id >= q->next_index && id != (size_t)-1) q->next_index = id + 1;
    spin_unlock(&q->lock);

    return new_node->index;
}

void *queue_get(lock_queue *q, size_t index)
{
    if (q == NULL) return NULL;
    spin_lock(&q->lock);
    lock_node *current = q->head;

    while (current != NULL)
    {
        if (current->index == index)
        {
            spin_unlock(&q->lock);
            return current->data;
        }
        current = current->next;
    }
    spin_unlock(&q->lock);
    return NULL;
}

void *queue_remove_at(lock_queue *q, size_t index)
{
    if (q == NULL) return NULL;
    spin_lock(&q->lock);
    lock_node *current  = q->head;

    while (current != NULL)
    {
        if (current->index == index)
        {
            void *handle = queue_remove_locked(q, current);
            spin_unlock(&q->lock);
            return handle;
        }
        current  = current->next;
    }
    spin_unlock(&q->lock);
    return NULL;
}

/* Caller must already hold q->lock. Used by the lazy allocator so that we do
 * not re-enter the spinlock when we already serialise against the queue. */
void *queue_remove_at_locked(lock_queue *q, size_t index)
{
    if (q == NULL) return NULL;
    for (lock_node *current = q->head; current != NULL; current = current->next)
    {
        if (current->index == index) return queue_remove_locked(q, current);
    }
    return NULL;
}

void *queue_remove_data(lock_queue *q, void *data)
{
    if (q == NULL || data == NULL) return NULL;
    spin_lock(&q->lock);
    lock_node *current = q->head;

    while (current != NULL)
    {
        if (current->data == data)
        {
            void *handle = queue_remove_locked(q, current);
            spin_unlock(&q->lock);
            return handle;
        }
        current = current->next;
    }
    spin_unlock(&q->lock);
    return NULL;
}

void *queue_remove_node(lock_queue *q, lock_node *node)
{
    if (q == NULL || node == NULL) return NULL;

    spin_lock(&q->lock);
    void *handle = queue_remove_locked(q, node);
    spin_unlock(&q->lock);
    return handle;
}

void *queue_dequeue(lock_queue *q)
{
    spin_lock(&q->lock);
    if (!q->head)
    {
        spin_unlock(&q->lock);
        return NULL;
    }
    lock_node *temp = q->head;
    void      *data = temp->data;
    q->head         = q->head->next;
    if (q->head != NULL) q->head->prev = NULL;
    if (!q->head) { q->tail = NULL; }
    if (q->rr_cursor == temp)
    {
        q->rr_cursor = q->head;
    }
    q->size--;
    spin_unlock(&q->lock);
    free(temp);
    return data;
}

lock_queue *queue_copy(lock_queue *src, void *(*data_copy)(void *))
{
    if (!src) return NULL;

    spin_lock(&src->lock);

    lock_queue *new_q = queue_init();
    if (!new_q)
    {
        spin_unlock(&src->lock);
        return NULL;
    }

    lock_node *current = src->head;
    while (current != NULL)
    {
        lock_node *new_node = (lock_node *)malloc(sizeof(lock_node));
        if (!new_node)
        {
            spin_unlock(&src->lock);
            queue_destroy(new_q);
            return NULL;
        }
        new_node->data  = data_copy != NULL ? data_copy(current->data) : current->data;
        new_node->index = current->index;
        new_node->next  = NULL;
        new_node->prev  = new_q->tail;

        if (!new_q->head)
        {
            new_node->prev = NULL;
            new_q->head = new_q->tail = new_node;
        }
        else
        {
            new_q->tail->next = new_node;
            new_q->tail       = new_node;
        }

        current = current->next;
    }

    new_q->size       = src->size;
    new_q->next_index = src->next_index;

    spin_unlock(&src->lock);
    return new_q;
}

void queue_iterate(lock_queue *q, void (*callback)(void *, void *), void *argument)
{
    spin_lock(&q->lock);
    lock_node *current = q->head;
    while (current)
    {
        callback(current->data, argument);
        current = current->next;
    }
    spin_unlock(&q->lock);
}

void queue_destroy(lock_queue *q)
{
    spin_lock(&q->lock);
    lock_node *current = q->head;
    while (current)
    {
        lock_node *next = current->next;
        free(current);
        current = next;
    }
    q->head = q->tail = q->rr_cursor = NULL;
    spin_unlock(&q->lock);
    free(q);
}
