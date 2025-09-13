#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bus.h"
#include "util.h"

typedef struct {
    // Registers
    uint8_t A, X, Y;    // Accumulator and index registers
    uint8_t S;          // Stack pointer
    uint16_t PC;        // Program counter
    uint8_t P;          // Status flags NV-BDIZC (U always set)

    // Wiring
    Bus *bus;

    // Interrupt lines
    bool nmi_line;
    bool irq_line;

    // Cycle count (since power on)
    uint64_t cycles;
} CPU;

void cpu_connect_bus(CPU *c, Bus *b);
void cpu_power_on(CPU *c);
void cpu_reset(CPU *c);
void cpu_irq(CPU *c);
void cpu_nmi(CPU *c);

// Execute one instruction and return cycles consumed
int cpu_step(CPU *c);

