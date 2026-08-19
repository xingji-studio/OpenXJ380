#include "atom_queue.h"
#include "krlibc.h"
static uint64_t load(uint64_t *addr) {
    uint64_t ret = 0;
    __asm__ volatile("lock xadd %[ret], %[addr]\n\t"
                     : [addr] "+m"(*addr), [ret] "+r"(ret)
                     :
                     : "memory");
    return ret;
}

static void store(uint64_t *addr, uint64_t value) {
    __asm__ volatile("lock xchg %[value], %[addr] \n\t"
                     : [addr] "+m"(*addr), [value] "+r"(value)
                     :
                     : "memory");
}

bool init_atom_queue(atom_queue *queue, uint8_t *buffer, uint64_t size) {
    if (queue == NULL || buffer == NULL || size == 0 || (size & (size - 1)) != 0) { return false; }
    memset(queue, 0, sizeof(atom_queue));
    queue->buf  = buffer;
    queue->mask = size - 1;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
    return true;
}

bool atom_push(atom_queue *queue, uint8_t data) {
    if (queue == NULL || queue->buf == NULL) return false;
    uint64_t head = load(&queue->head);
    uint64_t next = (((uint64_t)(head + 1U)) & queue->mask);
    if (next == load(&queue->tail)) return false;
    *(&queue->buf[head]) = data;
    store(&queue->head, next);
    queue->size++;
    return true;
}

int atom_pop(atom_queue *queue) {
    if (queue == NULL || queue->buf == NULL) return -1;
    uint64_t tail = load(&queue->tail);
    if (tail == load(&queue->head)) return -1;
    uint8_t data = queue->buf[tail];
    store(&queue->tail, (((uint64_t)(tail + 1U)) & queue->mask));
    queue->size--;
    return (int)data;
}

