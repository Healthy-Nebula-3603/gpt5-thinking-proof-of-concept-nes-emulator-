#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    // Buttons bit order: A, B, Select, Start, Up, Down, Left, Right
    uint8_t state;       // current buttons state
    uint8_t shift;       // shift register state
    bool strobe;         // strobe bit ($4016 bit 0)
} Controller;

void controller_reset(Controller *c);
void controller_set_state(Controller *c, uint8_t state);
void controller_write(Controller *c, uint8_t data);
uint8_t controller_read(Controller *c);

