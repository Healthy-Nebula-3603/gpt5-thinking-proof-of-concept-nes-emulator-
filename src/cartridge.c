#include "cartridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint8_t magic[4]; // 'N','E','S',0x1A
    uint8_t prg_rom_16k_units;
    uint8_t chr_rom_8k_units;
    uint8_t flags6;
    uint8_t flags7;
    uint8_t prg_ram_8k_units;
    uint8_t flags9;
    uint8_t flags10;
    uint8_t zero[5];
} INesHeader;

static bool header_is_nes(const INesHeader *h) {
    return h->magic[0]=='N' && h->magic[1]=='E' && h->magic[2]=='S' && h->magic[3]==0x1A;
}

int cartridge_load(const char *path, Cartridge *cart) {
    memset(cart, 0, sizeof(*cart));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    INesHeader h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return -2; }
    if (!header_is_nes(&h)) { fclose(f); return -3; }

    uint8_t mapper_lower = (h.flags6 >> 4) & 0x0F;
    uint8_t mapper_upper = (h.flags7 & 0xF0); // already high nibble in correct position
    cart->mapper = (uint8_t)(mapper_lower | mapper_upper);
    cart->trainer_present = (h.flags6 & 0x04) != 0;
    cart->battery = (h.flags6 & 0x02) != 0;
    cart->mirror = (h.flags6 & 0x08) ? MIRROR_FOUR : ((h.flags6 & 0x01) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL);

    if (cart->mapper != 0) { fclose(f); return -4; }

    if (cart->trainer_present) {
        // Skip trainer 512 bytes
        if (fseek(f, 512, SEEK_CUR) != 0) { fclose(f); return -5; }
    }

    cart->prg_rom_size = (uint32_t)h.prg_rom_16k_units * 16 * 1024u;
    cart->chr_size = (uint32_t)h.chr_rom_8k_units * 8 * 1024u;

    if (cart->prg_rom_size == 0) { fclose(f); return -6; }
    cart->prg_rom = (uint8_t*)malloc(cart->prg_rom_size);
    if (!cart->prg_rom) { fclose(f); return -6; }
    if (fread(cart->prg_rom, 1, cart->prg_rom_size, f) != cart->prg_rom_size) { fclose(f); return -7; }

    if (cart->chr_size == 0) {
        // Allocate 8KB CHR RAM if no CHR ROM present
        cart->chr_size = 8 * 1024u;
        cart->chr = (uint8_t*)calloc(1, cart->chr_size);
        cart->chr_is_ram = true;
        if (!cart->chr) { fclose(f); return -8; }
    } else {
        cart->chr = (uint8_t*)malloc(cart->chr_size);
        cart->chr_is_ram = false;
        if (!cart->chr) { fclose(f); return -9; }
        if (fread(cart->chr, 1, cart->chr_size, f) != cart->chr_size) { fclose(f); return -10; }
    }

    // Allocate 8KB PRG RAM for NROM (some ROMs may not use it; safe default)
    cart->prg_ram_size = 8 * 1024u;
    cart->prg_ram = (uint8_t*)calloc(1, cart->prg_ram_size);
    if (!cart->prg_ram) { fclose(f); return -11; }

    fclose(f);
    return 0;
}

void cartridge_free(Cartridge *cart) {
    if (!cart) return;
    free(cart->prg_rom); cart->prg_rom = NULL; cart->prg_rom_size = 0;
    free(cart->chr); cart->chr = NULL; cart->chr_size = 0; cart->chr_is_ram = false;
    free(cart->prg_ram); cart->prg_ram = NULL; cart->prg_ram_size = 0;
}

uint8_t cart_cpu_read(Cartridge *c, uint16_t addr) {
    if (!c) return 0;
    // $6000-$7FFF PRG RAM
    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size) return c->prg_ram[addr - 0x6000];
        return 0;
    }
    // $8000-$FFFF PRG ROM
    if (addr >= 0x8000) {
        if (!c->prg_rom || c->prg_rom_size == 0) return 0;
        uint32_t offset = (uint32_t)addr - 0x8000u;
        if (c->prg_rom_size == 16 * 1024u) {
            // Mirror 16KB into both banks
            offset %= 16 * 1024u;
        } else {
            offset %= c->prg_rom_size;
        }
        return c->prg_rom[offset];
    }
    return 0;
}

void cart_cpu_write(Cartridge *c, uint16_t addr, uint8_t data) {
    if (!c) return;
    // PRG RAM
    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size) c->prg_ram[addr - 0x6000] = data;
        return;
    }
    // Writes to PRG ROM ignored for NROM
    (void)c; (void)addr; (void)data;
}
