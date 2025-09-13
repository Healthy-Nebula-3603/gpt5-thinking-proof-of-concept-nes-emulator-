#include "nes.h"
#include <string.h>

void nes_init(NES *nes, bool enable_audio) {
    memset(nes, 0, sizeof(*nes));
    controller_reset(&nes->ctrl1);
    controller_reset(&nes->ctrl2);
    bus_init(&nes->bus, nes);
    ppu_power_on(&nes->ppu);
    // Important: power on CPU before connecting the bus; cpu_power_on zeroes the struct
    cpu_power_on(&nes->cpu);
    cpu_connect_bus(&nes->cpu, &nes->bus);
    // Try to init audio (optional)
    if (enable_audio) {
        apu_init(&nes->apu);
        if (nes->apu) apu_connect_bus(nes->apu, &nes->bus);
    } else {
        nes->apu = NULL;
    }
}

int nes_load_rom(NES *nes, const char *path) {
    int rc = cartridge_load(path, &nes->cart);
    if (rc == 0) {
        ppu_connect_cartridge(&nes->ppu, &nes->cart, nes->cart.mirror);
    }
    return rc;
}

void nes_reset(NES *nes) {
    // Reset CPU which also reads reset vector from PRG ROM via bus
    cpu_reset(&nes->cpu);
    // Safety: if reset vector is 0x0000 (often indicates missing PRG mapping), don't crash later
    if (nes->cpu.PC == 0x0000) {
        // Try typical NROM start at $8000 as a fallback
        nes->cpu.PC = 0x8000;
    }
}

void nes_run_cycles(NES *nes, int cycles) {
    int remaining = cycles;
    while (remaining > 0) {
        int used = cpu_step(&nes->cpu);
        if (used <= 0) used = 1; // safety
        // Tick PPU based on CPU cycles consumed
        ppu_tick_cpu_cycles(&nes->ppu, used);
        if (nes->ppu.nmi_pending) {
            nes->ppu.nmi_pending = false;
            nes->cpu.nmi_line = true;
        }
        if (nes->apu) {
            apu_tick_cpu_cycles(nes->apu, used);
            if (apu_frame_irq_pending(nes->apu) || apu_dmc_irq_pending(nes->apu)) {
                nes->cpu.irq_line = true;
            }
        }
        remaining -= used;
    }
}

int nes_step_instruction(NES *nes) {
    int used = cpu_step(&nes->cpu);
    if (used <= 0) used = 1;
    ppu_tick_cpu_cycles(&nes->ppu, used);
    if (nes->ppu.nmi_pending) { nes->ppu.nmi_pending = false; nes->cpu.nmi_line = true; }
    return used;
}
