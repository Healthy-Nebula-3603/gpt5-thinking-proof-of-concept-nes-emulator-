#pragma once
#include <stdint.h>
#include "cartridge.h"
#include "ppu.h"
#include "controller.h"

typedef struct NES NES; // fwd decl

typedef struct {
    // 2KB internal RAM, mirrored 0x0000-0x07FF through 0x1FFF
    uint8_t ram[2 * 1024];

    // Connections
    NES *nes;
} Bus;

void bus_init(Bus *b, NES *nes);
uint8_t bus_cpu_read(Bus *b, uint16_t addr);
void bus_cpu_write(Bus *b, uint16_t addr, uint8_t data);

