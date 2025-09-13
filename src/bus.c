#include "bus.h"
#include "cartridge.h"
#include "ppu.h"
#include "controller.h"
#include "nes.h"
#include <string.h>

void bus_init(Bus *b, NES *nes) {
    memset(b->ram, 0, sizeof(b->ram));
    b->nes = nes;
}

uint8_t bus_cpu_read(Bus *b, uint16_t addr) {
    if (!b || !b->nes) return 0;
    NES *nes = b->nes;
    if (addr <= 0x1FFF) {
        return b->ram[addr & 0x07FF];
    } else if (addr <= 0x3FFF) {
        // PPU registers $2000-$2007 mirrored
        return ppu_read_reg(&nes->ppu, (uint16_t)(0x2000 + (addr & 7)));
    } else if (addr == 0x4016) {
        return controller_read(&nes->ctrl1);
    } else if (addr == 0x4017) {
        return controller_read(&nes->ctrl2);
    } else if (addr >= 0x4000 && addr <= 0x4017) {
        // APU/IO reads (very minimal)
        // $4015 status: return enabled flag if APU exists
        if (addr == 0x4015) {
            return nes->apu ? apu_read(nes->apu, addr) : 0x00;
        }
        return 0;
    } else if (addr >= 0x6000) {
        return cart_cpu_read(&nes->cart, addr);
    } else {
        return 0;
    }
}

void bus_cpu_write(Bus *b, uint16_t addr, uint8_t data) {
    if (!b || !b->nes) return;
    NES *nes = b->nes;
    if (addr <= 0x1FFF) {
        b->ram[addr & 0x07FF] = data;
    } else if (addr <= 0x3FFF) {
        ppu_write_reg(&nes->ppu, (uint16_t)(0x2000 + (addr & 7)), data);
    } else if (addr == 0x4014) {
        // OAMDMA: copy 256 bytes from page data<<8 into PPU OAM starting at OAMADDR
        uint16_t base = (uint16_t)(data << 8);
        uint8_t start = nes->ppu.oamaddr;
        for (int i = 0; i < 256; ++i) {
            uint8_t v = bus_cpu_read(b, (uint16_t)(base + i));
            nes->ppu.oam[(uint8_t)(start + i)] = v;
        }
    } else if (addr == 0x4016) {
        controller_write(&nes->ctrl1, data);
        controller_write(&nes->ctrl2, data);
    } else if (addr >= 0x4000 && addr <= 0x4017) {
        // APU writes (very minimal)
        if (nes->apu) {
            apu_write(nes->apu, addr, data);
        }
    } else if (addr >= 0x6000) {
        cart_cpu_write(&nes->cart, addr, data);
    } else {
        (void)data;
    }
}
