#include "task/ipc.h"
#include "mm/heap.h"
#include "task/scheduler.h"

void ipc_send(pcb_t process, ipc_message_t message) {
    if (process == NULL || message == NULL) { return; }
    if (process->ipc_queue == NULL) { return; }
    /*
     * The queue owns the message after this call.  Do not touch message after
     * publishing it: an exec drain may dequeue and free it immediately.
     * The index field is retained for the ABI, but no kernel consumer relies
     * on it.
     */
    lock_queue_enqueue(process->ipc_queue, message);
}

void ipc_free_type(uint8_t type) {
    pcb_t pcb = get_current_task()->parent_group;
    // Another consumer or exec's queue drain may empty the queue between the
    // size snapshot and dequeue. NULL is therefore a normal concurrent result.
    for (size_t i = 0; i < pcb->ipc_queue->size; ++i) {
        ipc_message_t message = (ipc_message_t)queue_dequeue(pcb->ipc_queue);
        if (message == NULL) { break; }
        if (message->type == type) {
            free(message);
        } else {
            lock_queue_enqueue(pcb->ipc_queue, message);
        }
    }
}

ipc_message_t ipc_recv(uint8_t type) {
    pcb_t pcb = get_current_task()->parent_group;
    for (size_t i = 0; i < pcb->ipc_queue->size; ++i) {
        ipc_message_t message = (ipc_message_t)queue_dequeue(pcb->ipc_queue);
        if (message == NULL) { break; }
        if (message->type == type) { return message; }
        lock_queue_enqueue(pcb->ipc_queue, message);
    }
    return NULL;
}

ipc_message_t ipc_recv_wait(uint8_t type) {
    ipc_message_t message = NULL;
    do {
        scheduler_sleep_ns(1000000ULL);
        message = ipc_recv(type);
    } while (message == NULL);
    return message;
}

ipc_message_t ipc_recv_wait2(uint8_t type0, uint8_t type1) {
    ipc_message_t message = NULL;
    do {
        scheduler_sleep_ns(1000000ULL);
        message = ipc_recv(type0);
        if (message == NULL) { message = ipc_recv(type1); }
    } while (message == NULL);
    return message;
}
