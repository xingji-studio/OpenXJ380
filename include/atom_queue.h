#pragma once

#include <proto.hpp>

typedef struct {
    uint8_t *buf;
    uint64_t mask;
    uint64_t head;
    uint64_t tail;
    uint64_t size;
} atom_queue;

bool init_atom_queue(atom_queue *queue, uint8_t *buffer, uint64_t size);
bool atom_push(atom_queue *queue, uint8_t data);
int  atom_pop(atom_queue *queue);
