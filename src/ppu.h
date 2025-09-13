#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cartridge.h"

typedef struct {
    // Registers
    uint8_t ppuctrl;    // $2000
    uint8_t ppumask;    // $2001
    uint8_t ppustatus;  // $2002
    uint8_t oamaddr;    // $2003
    uint8_t oamdata;    // $2004
    uint8_t scroll_latch; // internal toggle for $2005/$2006
    uint8_t ppuaddr_hi;
    uint8_t ppuaddr_lo;
    uint8_t ppudata_buffer; // read buffer behavior

    // Timing
    int64_t cpu_cycles_accum;   // accumulate CPU cycles to approximate frames
    bool nmi_pending;           // NMI pending flag
    bool frame_ready;           // set at vblank; consumer resets

    // PPU memory
    uint8_t vram[2 * 1024];     // Nametables (2KB) with mirroring
    uint8_t palette[32];        // Palette RAM $3F00-$3F1F
    uint8_t oam[256];           // Object Attribute Memory
    uint8_t bg_opaque[256*240]; // Track BG non-transparent pixels for sprite priority
    uint8_t vram_addr_hi;       // internal address latch high/low for $2006
    uint8_t vram_addr_lo;
    uint16_t vram_addr;         // current VRAM address
    // Loopy scroll registers
    uint16_t v;                 // current VRAM address (loopy V)
    uint16_t t;                 // temporary VRAM address (loopy T)
    uint8_t x_fine;             // fine X scroll (0..7)
    uint8_t w;                  // write toggle for $2005/$2006 (0/1)

    // Scrolling (legacy helpers; prefer loopy regs above)
    uint8_t scroll_x;           // from $2005 first write
    uint8_t scroll_y;           // from $2005 second write

    // Cartridge link for CHR access and mirroring
    const Cartridge *cart;
    MirrorMode mirror;

    // Framebuffer (ARGB8888)
    uint32_t framebuffer[256 * 240];

    // Current position (for per-dot stepping)
    int scanline;
    int dot;
    bool odd_frame;

    // Background shifters and fetch latches
    uint16_t bg_shift_lo;
    uint16_t bg_shift_hi;
    uint16_t at_shift_lo;
    uint16_t at_shift_hi;
    uint8_t nt_byte;
    uint8_t at_byte;
    uint8_t bg_next_lo;
    uint8_t bg_next_hi;

    // Sprite evaluation for current scanline
    uint8_t spr_count;
    uint8_t spr_x[8];
    uint8_t spr_attr[8];
    uint8_t spr_lo[8];
    uint8_t spr_hi[8];
    uint8_t spr_index[8]; // original OAM index (for sprite 0 hit)

    // Next scanline sprite buffer
    uint8_t spr_count_next;
    uint8_t spr_x_next[8];
    uint8_t spr_attr_next[8];
    uint8_t spr_lo_next[8];
    uint8_t spr_hi_next[8];
    uint8_t spr_index_next[8];

} PPU;

void ppu_reset(PPU *p);
void ppu_power_on(PPU *p);

// Advance PPU time by CPU cycles; generate VBlank NMI at ~60Hz
void ppu_tick_cpu_cycles(PPU *p, int cycles);

// CPU-facing register access ($2000-$2007)
uint8_t ppu_read_reg(PPU *p, uint16_t reg);
void ppu_write_reg(PPU *p, uint16_t reg, uint8_t data);

// ROM integration
void ppu_connect_cartridge(PPU *p, const Cartridge *cart, MirrorMode mirror);

// Render background into framebuffer (very simplified). Returns pointer to ARGB pixels.
const uint32_t *ppu_render_frame(PPU *p);

// Debug controls
void ppu_set_debug(bool on);
