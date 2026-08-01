#ifndef PCSPK_H
#define PCSPK_H

#include <stdint.h>

void pcspk_play(uint32_t frequency);
void pcspk_stop();
int pcspk_init();
void pcspk_test();

#endif