#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MIRROR_HORIZONTAL = 0,
    MIRROR_VERTICAL = 1,
    MIRROR_FOUR = 2
} MirrorMode;

typedef struct {
    // Raw ROM/RAM
    uint8_t *prg_rom;       // PRG ROM data
    uint32_t prg_rom_size;  // size in bytes (16KB or 32KB multiples)

    uint8_t *chr;           // CHR ROM or CHR RAM
    uint32_t chr_size;      // size in bytes (0 => allocate 8KB CHR RAM)
    bool chr_is_ram;        // true if CHR is RAM (no CHR ROM in file)

    uint8_t *prg_ram;       // 8KB PRG RAM typical for NROM
    uint32_t prg_ram_size;

    // iNES header params
    uint8_t mapper;         // Only 0 supported (NROM)
    MirrorMode mirror;
    bool battery;
    bool trainer_present;
} Cartridge;

int cartridge_load(const char *path, Cartridge *cart);
void cartridge_free(Cartridge *cart);

// PRG mapping helpers (NROM)
uint8_t cart_cpu_read(Cartridge *c, uint16_t addr);
void cart_cpu_write(Cartridge *c, uint16_t addr, uint8_t data);
