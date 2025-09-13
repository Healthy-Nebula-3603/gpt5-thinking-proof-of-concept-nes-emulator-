#include "ppu.h"
#include "cartridge.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Forward declarations for helpers used before their definition
static uint8_t ppu_read_mem(PPU *p, uint16_t addr);
static void ppu_write_mem(PPU *p, uint16_t addr, uint8_t data);
static const uint32_t NES_PALETTE[64];

// Debug
static bool g_ppu_debug = false;
static int g_ppu_dbg_count = 0;
static const int g_ppu_dbg_limit = 400;

static inline void ppu_dbgf(const char *fmt, ...) {
    if (!g_ppu_debug) return;
    if (g_ppu_dbg_count >= g_ppu_dbg_limit) return;
    g_ppu_dbg_count++;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void ppu_set_debug(bool on) {
    g_ppu_debug = on; g_ppu_dbg_count = 0;
}

// (no per-scanline trace function in original)

// NTSC PPU timing
#define PPU_SCANLINES 262
#define PPU_DOTS_PER_LINE 341
// CPU:PPU = 1:3

void ppu_reset(PPU *p) {
    memset(p, 0, sizeof(*p));
    p->ppustatus = 0xA0; // typical power-up pattern (bits 7,5 set variably)
    p->v = 0; p->t = 0; p->x_fine = 0; p->w = 0;
    p->scanline = 0; p->dot = 0;
    p->odd_frame = false;
    p->bg_shift_lo = p->bg_shift_hi = p->at_shift_lo = p->at_shift_hi = 0;
    p->nt_byte = p->at_byte = p->bg_next_lo = p->bg_next_hi = 0;
}

void ppu_power_on(PPU *p) {
    ppu_reset(p);
}

static inline void inc_coarse_x(PPU *p) {
    if ((p->v & 0x001F) == 31) {
        p->v &= (uint16_t)~0x001F;         // coarse X = 0
        p->v ^= 0x0400;                    // switch horizontal nametable
    } else {
        p->v += 1;                         // increment coarse X
    }
}

static inline void inc_y(PPU *p) {
    if ((p->v & 0x7000) != 0x7000) {
        p->v += 0x1000;                    // increment fine Y
    } else {
        p->v &= (uint16_t)~0x7000;         // fine Y = 0
        uint16_t y = (uint16_t)((p->v & 0x03E0) >> 5); // coarse Y
        if (y == 29) {
            y = 0; p->v ^= 0x0800;         // switch vertical nametable
        } else if (y == 31) {
            y = 0;                         // coarse Y = 0, nametable not switched
        } else {
            y += 1;                        // increment coarse Y
        }
        p->v = (uint16_t)((p->v & ~0x03E0) | (y << 5));
    }
}

static inline void transfer_horizontal(PPU *p) {
    // v: ....F.. ...EDCBA = t: ....F.. ...EDCBA
    p->v = (uint16_t)((p->v & 0x7BE0) | (p->t & 0x041F));
}

static inline void transfer_vertical(PPU *p) {
    // v: IHGF.ED CBA..... = t: IHGF.ED CBA.....
    p->v = (uint16_t)((p->v & 0x041F) | (p->t & 0x7BE0));
}

static void ppu_step(PPU *p) {
    // Advance one PPU cycle (dot)
    if (!p) return;
    int scanline = p->scanline;
    int dot = p->dot;
    // Odd frame cycle skip: on pre-render line, if rendering enabled, skip one dot
    if (scanline == 261 && dot == 0) {
        if ((p->ppumask & 0x18) && p->odd_frame) {
            // Skip dot 0
            dot = 1;
        }
    }

    // VBlank start at scanline 241, dot 1
    if (scanline == 241 && dot == 1) {
        p->ppustatus |= 0x80; // set VBlank
        if (p->ppuctrl & 0x80) p->nmi_pending = true;
    }
    // Pre-render line clears VBlank
    if (scanline == 261 && dot == 1) {
        p->ppustatus &= (uint8_t)~0x80;
        p->ppustatus &= (uint8_t)~0x40; // clear sprite 0 hit
    }
    bool rendering_on = (p->ppumask & 0x18) != 0;
    bool visible_line = (scanline >= 0 && scanline < 240);
    bool pre_render = (scanline == 261);

    // Copy prepared sprites for this scanline at dot 1
    if (rendering_on && visible_line && dot == 1) {
        p->spr_count = p->spr_count_next;
        for (int i = 0; i < p->spr_count; ++i) {
            p->spr_x[i] = p->spr_x_next[i];
            p->spr_attr[i] = p->spr_attr_next[i];
            p->spr_lo[i] = p->spr_lo_next[i];
            p->spr_hi[i] = p->spr_hi_next[i];
            p->spr_index[i] = p->spr_index_next[i];
        }
        // Preload background shifters from last prefetch (dots 321-336) so first tile renders
        p->bg_shift_lo = (uint16_t)(p->bg_next_lo << 8);
        p->bg_shift_hi = (uint16_t)(p->bg_next_hi << 8);
        // Preload attribute shifters for current tile
        uint8_t coarse_x = (uint8_t)(p->v & 0x1F);
        uint8_t coarse_y = (uint8_t)((p->v >> 5) & 0x1F);
        uint8_t quadrant = (uint8_t)(((coarse_y & 0x02) << 1) | (coarse_x & 0x02));
        uint8_t attr = p->at_byte;
        uint8_t palette_bits = 0;
        switch (quadrant) {
            case 0x00: palette_bits = (uint8_t)(attr & 0x03); break;
            case 0x02: palette_bits = (uint8_t)((attr >> 2) & 0x03); break;
            case 0x04: palette_bits = (uint8_t)((attr >> 4) & 0x03); break;
            case 0x06: palette_bits = (uint8_t)((attr >> 6) & 0x03); break;
            default: palette_bits = (uint8_t)(attr & 0x03); break;
        }
        p->at_shift_lo = (uint16_t)((palette_bits & 0x01) ? 0xFF00 : 0x0000);
        p->at_shift_hi = (uint16_t)((palette_bits & 0x02) ? 0xFF00 : 0x0000);
    }

    // Visible pixels: 0-239 scanlines, dots 1..256
    if (rendering_on && visible_line && dot >= 1 && dot <= 256) {
        int x = dot - 1;
        int y = scanline;
        // Get background pixel from shifters BEFORE shifting (PPU renders then shifts)
        uint8_t bg_pix = 0;
        uint8_t pal_select = 0;
        if (p->ppumask & 0x08) {
            uint16_t mask = (uint16_t)(0x8000 >> (p->x_fine & 7));
            uint8_t b0 = (uint8_t)((p->bg_shift_lo & mask) ? 1 : 0);
            uint8_t b1 = (uint8_t)((p->bg_shift_hi & mask) ? 1 : 0);
            bg_pix = (uint8_t)((b1 << 1) | b0);
            uint8_t a0 = (uint8_t)((p->at_shift_lo & mask) ? 1 : 0);
            uint8_t a1 = (uint8_t)((p->at_shift_hi & mask) ? 1 : 0);
            pal_select = (uint8_t)((a1 << 1) | a0);
        }
        // PPUMASK left column masking
        if (x < 8 && (p->ppumask & 0x02) == 0) { bg_pix = 0; }
        uint8_t bg_palette_index = (bg_pix == 0) ? ppu_read_mem(p, 0x3F00) : ppu_read_mem(p, (uint16_t)(0x3F00 + pal_select * 4 + bg_pix));
        uint32_t bg_color = NES_PALETTE[bg_palette_index & 0x3F];
        bool bg_opaque = (bg_pix != 0) && ((p->ppumask & 0x08) != 0);

        // PPUMASK bits: show background/sprites and left 8 masking
        bool show_bg = (p->ppumask & 0x08) != 0;
        bool show_spr = (p->ppumask & 0x10) != 0;
        bool mask_left_spr = (p->ppumask & 0x04) == 0; // if 0, mask left 8 pixels
        uint32_t color = bg_color;

        // Sprites at this scanline: brute-force per-pixel search
        uint32_t sp_color = 0; bool sp_opaque = false; bool sp0 = false; bool sp_behind = false;
        if (show_spr && (!(x < 8 && mask_left_spr))) {
            for (int i = 0; i < p->spr_count; ++i) {
                if (p->spr_x[i] > 0) continue; // not yet reached
                uint8_t p0s = (uint8_t)((p->spr_lo[i] & 0x80) ? 1 : 0);
                uint8_t p1s = (uint8_t)((p->spr_hi[i] & 0x80) ? 1 : 0);
                uint8_t pix = (uint8_t)((p1s << 1) | p0s);
                if (pix == 0) continue;
                uint8_t attr = p->spr_attr[i];
                sp_opaque = true; sp0 = (p->spr_index[i] == 0); sp_behind = (attr & 0x20) != 0;
                uint16_t paladdr = (uint16_t)(0x3F10 + (attr & 0x03) * 4 + pix);
                if ((paladdr & 0x13) == 0x10) paladdr = 0x3F00;
                uint8_t pi = ppu_read_mem(p, paladdr);
                sp_color = NES_PALETTE[pi & 0x3F];
                break;
            }
        }
        bool use_sprite = sp_opaque && !(sp_behind && bg_opaque);
        if (!show_bg) { bg_opaque = false; }
        if (!show_spr) { use_sprite = false; }
        color = (use_sprite ? sp_color : bg_color);
        p->framebuffer[y * 256 + x] = color;
        p->bg_opaque[y * 256 + x] = bg_opaque ? 1 : 0;
        if (sp0 && use_sprite && bg_opaque && x != 255) p->ppustatus |= 0x40; // sprite 0 hit

        // Now shift background shifters for next pixel
        if (p->ppumask & 0x08) {
            p->bg_shift_lo <<= 1;
            p->bg_shift_hi <<= 1;
            p->at_shift_lo <<= 1;
            p->at_shift_hi <<= 1;
        }
        // Advance sprite shifters/counters
        if (show_spr) {
            for (int i = 0; i < p->spr_count; ++i) {
                if (p->spr_x[i] > 0) {
                    p->spr_x[i]--;
                } else {
                    p->spr_lo[i] <<= 1;
                    p->spr_hi[i] <<= 1;
                }
            }
        }
    }

    // Background tile fetch pipeline and shifter reloads
    if (rendering_on && (visible_line || pre_render) && ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336))) {
        switch (dot % 8) {
            case 1: {
                // Fetch nametable byte
                uint16_t addr = (uint16_t)(0x2000 | (p->v & 0x0FFF));
                p->nt_byte = ppu_read_mem(p, addr);
                break;
            }
            case 3: {
                // Fetch attribute byte
                uint16_t addr = (uint16_t)(0x23C0 | (p->v & 0x0C00) | ((p->v >> 4) & 0x38) | ((p->v >> 2) & 0x07));
                p->at_byte = ppu_read_mem(p, addr);
                break;
            }
            case 5: {
                // Fetch low pattern byte
                uint16_t fine_y = (uint16_t)((p->v >> 12) & 0x7);
                uint16_t base = (p->ppuctrl & 0x10) ? 0x1000 : 0x0000;
                uint16_t addr = (uint16_t)(base + ((uint16_t)p->nt_byte * 16) + fine_y);
                p->bg_next_lo = ppu_read_mem(p, addr);
                break;
            }
            case 7: {
                // Fetch high pattern byte
                uint16_t fine_y = (uint16_t)((p->v >> 12) & 0x7);
                uint16_t base = (p->ppuctrl & 0x10) ? 0x1000 : 0x0000;
                uint16_t addr = (uint16_t)(base + ((uint16_t)p->nt_byte * 16) + fine_y + 8);
                p->bg_next_hi = ppu_read_mem(p, addr);
                break;
            }
            case 0: {
                // Reload shifters: load next pattern bytes into upper 8 bits
                p->bg_shift_lo = (uint16_t)((p->bg_shift_lo & 0x00FF) | ((uint16_t)p->bg_next_lo << 8));
                p->bg_shift_hi = (uint16_t)((p->bg_shift_hi & 0x00FF) | ((uint16_t)p->bg_next_hi << 8));
                // Attribute bits for this tile
                uint8_t attr = p->at_byte;
                uint8_t coarse_x = (uint8_t)(p->v & 0x1F);
                uint8_t coarse_y = (uint8_t)((p->v >> 5) & 0x1F);
                uint8_t quadrant = (uint8_t)(((coarse_y & 0x02) << 1) | (coarse_x & 0x02));
                uint8_t palette_bits = 0;
                switch (quadrant) {
                    case 0x00: palette_bits = (uint8_t)(attr & 0x03); break;         // top-left
                    case 0x02: palette_bits = (uint8_t)((attr >> 2) & 0x03); break;   // top-right
                    case 0x04: palette_bits = (uint8_t)((attr >> 4) & 0x03); break;   // bottom-left
                    case 0x06: palette_bits = (uint8_t)((attr >> 6) & 0x03); break;   // bottom-right
                    default: palette_bits = (uint8_t)(attr & 0x03); break;
                }
                uint16_t fill = (palette_bits & 0x01) ? 0xFF00 : 0x0000;
                p->at_shift_lo = (uint16_t)((p->at_shift_lo & 0x00FF) | fill);
                fill = (palette_bits & 0x02) ? 0xFF00 : 0x0000;
                p->at_shift_hi = (uint16_t)((p->at_shift_hi & 0x00FF) | fill);
                // Important: do NOT increment coarse X at dot 256.
                // On the real PPU, dot 256 increments only Y; horizontal
                // scroll (coarse X) is not advanced until later.
                // Incrementing here causes an extra column from the next
                // nametable to bleed in on the right edge.
                if (dot != 256) inc_coarse_x(p);
                break;
            }
        }
    }
    // Dot 256: increment Y
    if (rendering_on && visible_line && dot == 256) inc_y(p);
    // Dot 257: copy horizontal bits from t to v
    if (rendering_on && (visible_line || pre_render) && dot == 257) transfer_horizontal(p);
    // Sprite evaluation for next scanline at dot 257
    if (rendering_on && (visible_line || pre_render) && dot == 257) {
        int next_scan = (scanline + 1) % PPU_SCANLINES;
        p->spr_count_next = 0;
        if (next_scan < 240) {
            p->ppustatus &= (uint8_t)~0x20; // clear overflow
            bool sprites_8x16 = (p->ppuctrl & 0x20) != 0;
            int height = sprites_8x16 ? 16 : 8;
            for (int n = 0; n < 64; ++n) {
                int sy = p->oam[n * 4 + 0] + 1;
                int row = next_scan - sy;
                if (row < 0 || row >= height) continue;
                if (p->spr_count_next < 8) {
                    uint8_t tile = p->oam[n * 4 + 1];
                    uint8_t attr = p->oam[n * 4 + 2];
                    uint8_t sx = p->oam[n * 4 + 3];
                    bool flip_h = (attr & 0x40) != 0;
                    bool flip_v = (attr & 0x80) != 0;
                    int r = row; if (flip_v) r = height - 1 - r;
                    uint8_t lo, hi;
                    if (!sprites_8x16) {
                        uint16_t base = (p->ppuctrl & 0x08) ? 0x1000 : 0x0000;
                        uint16_t addr = (uint16_t)(base + tile * 16 + (r & 7));
                        lo = ppu_read_mem(p, addr);
                        hi = ppu_read_mem(p, (uint16_t)(addr + 8));
                    } else {
                        int top_tile_index = tile & 0xFE;
                        uint16_t top_base = (tile & 1) ? 0x1000 : 0x0000;
                        int tile_row = (r < 8) ? 0 : 1;
                        int in_row = r & 7;
                        uint16_t base = (uint16_t)(top_base + (top_tile_index + tile_row) * 16);
                        lo = ppu_read_mem(p, (uint16_t)(base + in_row));
                        hi = ppu_read_mem(p, (uint16_t)(base + in_row + 8));
                    }
                    if (flip_h) {
                        uint8_t rl=0, rh=0; for (int b=0;b<8;++b){ rl=(uint8_t)((rl<<1)|((lo>>b)&1)); rh=(uint8_t)((rh<<1)|((hi>>b)&1)); }
                        lo=rl; hi=rh;
                    }
                    int i = p->spr_count_next++;
                    p->spr_x_next[i] = sx;
                    p->spr_attr_next[i] = attr;
                    p->spr_lo_next[i] = lo;
                    p->spr_hi_next[i] = hi;
                    p->spr_index_next[i] = (uint8_t)n;
                } else {
                    p->ppustatus |= 0x20;
                    break;
                }
            }
        }
    }
    // Dots 280-304 on pre-render: copy vertical bits from t to v
    if (rendering_on && pre_render && dot >= 280 && dot <= 304) transfer_vertical(p);

    // Advance dot
    dot++;
    if (dot >= PPU_DOTS_PER_LINE) {
        dot = 0; scanline++;
        if (scanline >= PPU_SCANLINES) { scanline = 0; p->frame_ready = true; p->odd_frame = !p->odd_frame; }
        // end of line
    }
    p->scanline = scanline; p->dot = dot;
}

void ppu_tick_cpu_cycles(PPU *p, int cycles) {
    int ppu_cycles = cycles * 3;
    for (int i = 0; i < ppu_cycles; ++i) ppu_step(p);
}

static inline uint16_t mirror_nt_addr(PPU *p, uint16_t addr) {
    // Map $2000-$2FFF to 2KB VRAM based on mirroring
    uint16_t a = (uint16_t)((addr - 0x2000) & 0x0FFF); // 4*1KB region
    uint16_t table = (uint16_t)(a / 0x0400); // 0..3
    uint16_t offset = (uint16_t)(a & 0x03FF);
    switch (p->mirror) {
        case MIRROR_VERTICAL:
            if (table == 2) table = 0; else if (table == 3) table = 1;
            break;
        case MIRROR_HORIZONTAL:
            if (table == 1) table = 0; else if (table == 3) table = 2;
            break;
        case MIRROR_FOUR:
        default:
            // 4-screen: map 0..3 to 0..3 collapsed into 2KB (not fully supported), fallback to vertical
            if (table == 2) table = 0; else if (table == 3) table = 1;
            break;
    }
    return (uint16_t)(table * 0x0400 + offset);
}

static uint8_t ppu_read_mem(PPU *p, uint16_t addr) {
    addr &= 0x3FFF; // PPU address space wraps every 16KB
    if (addr < 0x2000) {
        // Pattern tables from CHR
        if (!p->cart || !p->cart->chr || p->cart->chr_size == 0) return 0;
        return p->cart->chr[addr % p->cart->chr_size];
    } else if (addr < 0x3F00) {
        uint16_t nt = mirror_nt_addr(p, addr);
        return p->vram[nt];
    } else if (addr < 0x4000) {
        uint16_t pal = (uint16_t)((addr - 0x3F00) & 0x1F);
        // Handle mirrors of universal background color
        if (pal == 0x10) pal = 0x00;
        if (pal == 0x14) pal = 0x04;
        if (pal == 0x18) pal = 0x08;
        if (pal == 0x1C) pal = 0x0C;
        return p->palette[pal];
    } else {
        return 0;
    }
}

static void ppu_write_mem(PPU *p, uint16_t addr, uint8_t data) {
    addr &= 0x3FFF; // PPU address space wraps every 16KB
    if (addr < 0x2000) {
        // CHR RAM writes only if cartridge has CHR RAM
        if (p->cart && p->cart->chr && p->cart->chr_is_ram) {
            p->cart->chr[addr % p->cart->chr_size] = data;
        }
    } else if (addr < 0x3F00) {
        uint16_t nt = mirror_nt_addr(p, addr);
        p->vram[nt] = data;
    } else if (addr < 0x4000) {
        uint16_t pal = (uint16_t)((addr - 0x3F00) & 0x1F);
        if (pal == 0x10) pal = 0x00;
        if (pal == 0x14) pal = 0x04;
        if (pal == 0x18) pal = 0x08;
        if (pal == 0x1C) pal = 0x0C;
        p->palette[pal] = data;
        if (g_ppu_debug && g_ppu_dbg_count < g_ppu_dbg_limit) {
            ppu_dbgf("PPU: PALETTE[%02X] <= %02X\n", pal, data);
        }
    } else {
        (void)data;
    }
}

uint8_t ppu_read_reg(PPU *p, uint16_t reg) {
    reg &= 7; // mirrors every 8 bytes
    switch (reg) {
        case 2: { // PPUSTATUS
            uint8_t v = p->ppustatus;
            // reading PPUSTATUS clears vblank and the address latch toggle
            p->ppustatus &= (uint8_t)~0x80;
            p->scroll_latch = 0;
            p->w = 0;
            return v;
        }
        case 4: { // OAMDATA
            // Reads OAM at OAMADDR
            return p->oam[p->oamaddr];
        }
        case 7: { // PPUDATA (stubbed: buffered reads)
            // Buffered reads except palette range
            uint8_t ret;
            if (p->vram_addr >= 0x3F00 && p->vram_addr < 0x4000) {
                ret = p->palette[(p->vram_addr - 0x3F00) & 0x1F];
            } else {
                ret = p->ppudata_buffer;
                p->ppudata_buffer = ppu_read_mem(p, p->vram_addr);
            }
            uint16_t inc = (p->ppuctrl & 0x04) ? 32 : 1;
            p->vram_addr = (uint16_t)(p->vram_addr + inc);
            p->ppuaddr_hi = (uint8_t)(p->vram_addr >> 8);
            p->ppuaddr_lo = (uint8_t)(p->vram_addr & 0xFF);
            return ret;
        }
        default:
            return 0;
    }
}

void ppu_write_reg(PPU *p, uint16_t reg, uint8_t data) {
    reg &= 7; // mirrors
    switch (reg) {
        case 0: // PPUCTRL
            p->ppuctrl = data;
            // Update t nametable bits (10-11)
            p->t = (uint16_t)((p->t & 0xF3FF) | ((data & 0x03) << 10));
            ppu_dbgf("PPU: $2000 (PPUCTRL) <= %02X  t=%04X\n", data, p->t);
            break;
        case 1: // PPUMASK
            p->ppumask = data;
            ppu_dbgf("PPU: $2001 (PPUMASK) <= %02X  showBG=%d showSP=%d\n", data, (data&0x08)!=0, (data&0x10)!=0);
            break;
        case 3: p->oamaddr = data; break;           // OAMADDR
        case 4: { // OAMDATA
            p->oam[p->oamaddr++] = data;            // auto-increment
            break;
        }
        case 5: { // PPUSCROLL
            if (p->w == 0) {
                // X scroll
                p->x_fine = (uint8_t)(data & 0x07);
                // coarse X bits 0-4
                p->t = (uint16_t)((p->t & 0xFFE0) | (data >> 3));
                p->w = 1;
                ppu_dbgf("PPU: $2005 (SCROLL) X <= %02X  t=%04X x_fine=%d\n", data, p->t, p->x_fine);
            } else {
                // Y scroll
                // fine Y bits 12-14
                p->t = (uint16_t)((p->t & 0x8FFF) | ((data & 0x07) << 12));
                // coarse Y bits 5-9
                p->t = (uint16_t)((p->t & 0xFC1F) | ((data & 0xF8) << 2));
                p->w = 0;
                ppu_dbgf("PPU: $2005 (SCROLL) Y <= %02X  t=%04X\n", data, p->t);
            }
            break;
        }
        case 6: { // PPUADDR
            if (p->w == 0) {
                p->t = (uint16_t)((p->t & 0x00FF) | ((data & 0x3F) << 8));
                p->w = 1;
                ppu_dbgf("PPU: $2006 (ADDR) hi <= %02X  t=%04X\n", data, p->t);
            } else {
                p->t = (uint16_t)((p->t & 0xFF00) | data);
                p->v = p->t;
                p->w = 0;
                ppu_dbgf("PPU: $2006 (ADDR) lo <= %02X  v=%04X\n", data, p->v);
            }
            break;
        }
        case 7: { // PPUDATA
            if (g_ppu_debug) {
                ppu_dbgf("PPU: $2007 (DATA) write @ %04X <= %02X\n", p->v, data);
            }
            ppu_write_mem(p, p->v, data);
            uint16_t inc = (p->ppuctrl & 0x04) ? 32 : 1;
            p->v = (uint16_t)(p->v + inc);
            p->ppuaddr_hi = (uint8_t)(p->v >> 8);
            p->ppuaddr_lo = (uint8_t)(p->v & 0xFF);
            break;
        }
        default:
            break;
    }
}

void ppu_connect_cartridge(PPU *p, const Cartridge *cart, MirrorMode mirror) {
    p->cart = cart;
    p->mirror = mirror;
}

// Classic NES palette (approx.)
static const uint32_t NES_PALETTE[64] = {
    0xFF757575,0xFF271B8F,0xFF0000AB,0xFF47009F,0xFF8F0077,0xFFAB0013,0xFFA70000,0xFF7F0B00,
    0xFF432F00,0xFF004700,0xFF005100,0xFF003F17,0xFF1B3F5F,0xFF000000,0xFF000000,0xFF000000,
    0xFFBCBCBC,0xFF0073EF,0xFF233BEF,0xFF8300F3,0xFFBF00BF,0xFFE7005B,0xFFDB2B00,0xFFCB4F0F,
    0xFF8B7300,0xFF009700,0xFF00AB00,0xFF00933B,0xFF00838B,0xFF000000,0xFF000000,0xFF000000,
    0xFFFFFFFF,0xFF3FBFFF,0xFF5F73FF,0xFF9F4BFF,0xFFFF3FFF,0xFFFF2747,0xFFFF7B5F,0xFFFFA347,
    0xFFF3BF3F,0xFF83D313,0xFF4FDF4B,0xFF58F898,0xFF00EBDB,0xFF000000,0xFF000000,0xFF000000,
    0xFFFFFFFF,0xFFABE7FF,0xFFC7D7FF,0xFFD7CBFF,0xFFFFC7FF,0xFFFFC7DB,0xFFFFBFAB,0xFFFFDBAB,
    0xFFFFE7A3,0xFFE3FFA3,0xFFABF3BF,0xFFB3FFCF,0xFF9FFFF3,0xFF000000,0xFF000000,0xFF000000
};

const uint32_t *ppu_render_frame(PPU *p) {
    // Background with loopy regs (approximate)
    uint16_t pattern_base = (p->ppuctrl & 0x10) ? 0x1000 : 0x0000; // BG pattern table
    uint16_t v = p->v;
    int coarse_x_start = v & 0x1F;
    int coarse_y_start = (v >> 5) & 0x1F; // 0..29 used
    int nt_x_start = (v >> 10) & 1;
    int nt_y_start = (v >> 11) & 1;
    int fine_y_start = (v >> 12) & 0x07;
    int fine_x = p->x_fine & 7;

    for (int y = 0; y < 240; ++y) {
        int wy = fine_y_start + y;
        int ty_add = wy >> 3;
        int fy = wy & 7;
        int cy = coarse_y_start + ty_add;
        int nt_y = (nt_y_start + (cy / 30)) & 1;
        int row = cy % 30;
        for (int x = 0; x < 256; ++x) {
            int wx = fine_x + x;
            int tx_add = wx >> 3;
            int fx = wx & 7;
            int cx = coarse_x_start + tx_add;
            int nt_x = (nt_x_start + (cx / 32)) & 1;
            int col = cx % 32;
            uint16_t nametable = (uint16_t)(0x2000 + (nt_y * 2 + nt_x) * 0x400);
            uint16_t nt_addr = (uint16_t)(nametable + row * 32 + col);
            uint8_t tile = ppu_read_mem(p, nt_addr);
            // Attribute
            uint16_t at_addr = (uint16_t)(nametable + 0x3C0 + (row / 4) * 8 + (col / 4));
            uint8_t at = ppu_read_mem(p, at_addr);
            int sh = ((row % 4) / 2) * 4 + ((col % 4) / 2) * 2; // choose quadrant
            uint8_t pal_select = (uint8_t)((at >> sh) & 0x03);
            // Pattern fetch
            uint16_t pt_addr = (uint16_t)(pattern_base + tile * 16 + fy);
            uint8_t lo = ppu_read_mem(p, pt_addr);
            uint8_t hi = ppu_read_mem(p, (uint16_t)(pt_addr + 8));
            int bit = 7 - fx;
            uint8_t p0 = (uint8_t)((lo >> bit) & 1);
            uint8_t p1 = (uint8_t)((hi >> bit) & 1);
            uint8_t pix = (uint8_t)((p1 << 1) | p0);
            uint8_t palette_index = (pix == 0) ? ppu_read_mem(p, 0x3F00) : ppu_read_mem(p, (uint16_t)(0x3F00 + pal_select * 4 + pix));
            uint32_t color = NES_PALETTE[palette_index & 0x3F];
            int idx = y * 256 + x;
            p->framebuffer[idx] = color;
            p->bg_opaque[idx] = (pix != 0) ? 1 : 0;
        }
    }

    // Sprites: 8x8 and 8x16 with priority
    bool sprites_8x16 = (p->ppuctrl & 0x20) != 0;
    uint16_t spr_pattern_base = (p->ppuctrl & 0x08) ? 0x1000 : 0x0000; // ignored in 8x16
    for (int n = 0; n < 64; ++n) {
        uint8_t sy = p->oam[n * 4 + 0];
        uint8_t tile = p->oam[n * 4 + 1];
        uint8_t attr = p->oam[n * 4 + 2];
        uint8_t sx = p->oam[n * 4 + 3];

        int palette = attr & 0x03; // 0..3
        bool priority_behind = (attr & 0x20) != 0; // 1=behind bg
        bool flip_h = (attr & 0x40) != 0;
        bool flip_v = (attr & 0x80) != 0;
        int top = sy + 1;
        if (top >= 240) continue;
        if (!sprites_8x16) {
            for (int row = 0; row < 8; ++row) {
                int src_row = flip_v ? (7 - row) : row;
                uint8_t lo = ppu_read_mem(p, (uint16_t)(spr_pattern_base + tile * 16 + src_row));
                uint8_t hi = ppu_read_mem(p, (uint16_t)(spr_pattern_base + tile * 16 + src_row + 8));
                int py = top + row; if (py < 0 || py >= 240) continue;
                for (int col = 0; col < 8; ++col) {
                    int bit = flip_h ? col : (7 - col);
                    uint8_t p0 = (uint8_t)((lo >> bit) & 1);
                    uint8_t p1 = (uint8_t)((hi >> bit) & 1);
                    uint8_t spx = (uint8_t)((p1 << 1) | p0);
                    if (spx == 0) continue;
                    int px = (int)sx + col; if (px < 0 || px >= 256) continue;
                    int idx = py * 256 + px;
                    if (priority_behind && p->bg_opaque[idx]) continue;
                    uint16_t paladdr = (uint16_t)(0x3F10 + palette * 4 + spx);
                    if ((paladdr & 0x13) == 0x10) paladdr = 0x3F00;
                    uint8_t palette_index = ppu_read_mem(p, paladdr);
                    p->framebuffer[idx] = NES_PALETTE[palette_index & 0x3F];
                    // Sprite 0 hit (approximate)
                    if (n == 0 && p->bg_opaque[idx]) {
                        p->ppustatus |= 0x40;
                    }
                }
            }
        } else {
            // 8x16: tile index selects table by bit0; two tiles stacked
            int top_tile_index = tile & 0xFE;
            uint16_t top_base = (tile & 1) ? 0x1000 : 0x0000;
            for (int row = 0; row < 16; ++row) {
                int tile_row = row < 8 ? 0 : 1;
                int in_row = row & 7;
                int src_row = flip_v ? (15 - row) : row;
                int src_tile_row = src_row < 8 ? 0 : 1;
                int src_in_row = src_row & 7;
                uint16_t base = top_base + (top_tile_index + src_tile_row) * 16;
                uint8_t lo = ppu_read_mem(p, (uint16_t)(base + src_in_row));
                uint8_t hi = ppu_read_mem(p, (uint16_t)(base + src_in_row + 8));
                int py = top + row; if (py < 0 || py >= 240) continue;
                for (int col = 0; col < 8; ++col) {
                    int bit = flip_h ? col : (7 - col);
                    uint8_t p0 = (uint8_t)((lo >> bit) & 1);
                    uint8_t p1 = (uint8_t)((hi >> bit) & 1);
                    uint8_t spx = (uint8_t)((p1 << 1) | p0);
                    if (spx == 0) continue;
                    int px = (int)sx + col; if (px < 0 || px >= 256) continue;
                    int idx = py * 256 + px;
                    if (priority_behind && p->bg_opaque[idx]) continue;
                    uint16_t paladdr = (uint16_t)(0x3F10 + palette * 4 + spx);
                    if ((paladdr & 0x13) == 0x10) paladdr = 0x3F00;
                    uint8_t palette_index = ppu_read_mem(p, paladdr);
                    p->framebuffer[idx] = NES_PALETTE[palette_index & 0x3F];
                    if (n == 0 && p->bg_opaque[idx]) {
                        p->ppustatus |= 0x40;
                    }
                }
            }
        }
    }
    return p->framebuffer;
}
