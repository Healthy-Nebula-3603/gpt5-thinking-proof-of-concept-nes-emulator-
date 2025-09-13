#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"
#include "ppu.h"
#include "bus.h"
#include "cartridge.h"
#include "controller.h"
#include "apu.h"

typedef struct NES {
    CPU cpu;
    PPU ppu;
    Bus bus;
    Cartridge cart;
    Controller ctrl1, ctrl2;
    APU *apu;

    bool running;
} NES;

int nes_load_rom(NES *nes, const char *path);
void nes_init(NES *nes, bool enable_audio);
void nes_reset(NES *nes);
// Run a rough number of CPU cycles (will tick PPU alongside)
void nes_run_cycles(NES *nes, int cycles);
// Run a single instruction; returns cycles consumed
int nes_step_instruction(NES *nes);
