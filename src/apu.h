#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bus.h"

typedef struct APU APU; // opaque

bool apu_init(APU **out);
void apu_shutdown(APU **out);

// Very minimal square wave on pulse 1 controlled by $4000, $4002, $4003, $4015
void apu_write(APU *a, uint16_t addr, uint8_t data);
uint8_t apu_read(APU *a, uint16_t addr);

// Tick APU frame sequencer and counters by CPU cycles
void apu_tick_cpu_cycles(APU *a, int cpu_cycles);

// Connect bus for DMC memory fetches
void apu_connect_bus(APU *a, Bus *bus);

// Query IRQ flags (do not clear; reading $4015 clears on hardware)
bool apu_frame_irq_pending(APU *a);
bool apu_dmc_irq_pending(APU *a);
