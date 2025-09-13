#include "controller.h"

void controller_reset(Controller *c) {
    c->state = 0;
    c->shift = 0;
    c->strobe = false;
}

void controller_set_state(Controller *c, uint8_t state) {
    c->state = state;
}

void controller_write(Controller *c, uint8_t data) {
    bool new_strobe = (data & 0x01) != 0;
    // Rising edge or strobe high keeps loading
    c->strobe = new_strobe;
    if (c->strobe) {
        c->shift = c->state;
    }
}

uint8_t controller_read(Controller *c) {
    uint8_t ret = (c->shift & 0x01);
    if (!c->strobe) {
        c->shift = (uint8_t)((c->shift >> 1) | 0x80); // ones after exhausted as per behavior
    }
    return ret | 0x40; // upper bits typically 1 on real hardware
}

