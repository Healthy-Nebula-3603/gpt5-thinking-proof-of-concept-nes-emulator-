// Simple helpers shared across modules
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define UNUSED(x) (void)(x)

// Status flags
#define FLAG_C 0x01
#define FLAG_Z 0x02
#define FLAG_I 0x04
#define FLAG_D 0x08
#define FLAG_B 0x10
#define FLAG_U 0x20
#define FLAG_V 0x40
#define FLAG_N 0x80

static inline void set_bit_u8(uint8_t *v, uint8_t mask, bool on) {
    if (on) *v |= mask; else *v &= (uint8_t)~mask;
}

static inline bool get_bit_u8(uint8_t v, uint8_t mask) { return (v & mask) != 0; }

static inline uint16_t make16(uint8_t lo, uint8_t hi) { return (uint16_t)lo | ((uint16_t)hi << 8); }

static inline uint8_t lo8(uint16_t v) { return (uint8_t)(v & 0xFF); }
static inline uint8_t hi8(uint16_t v) { return (uint8_t)((v >> 8) & 0xFF); }

static inline bool page_crossed(uint16_t a, uint16_t b) { return (a & 0xFF00) != (b & 0xFF00); }

