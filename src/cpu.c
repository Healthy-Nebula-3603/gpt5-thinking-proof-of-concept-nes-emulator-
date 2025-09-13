#include "cpu.h"
#include "util.h"
#include <string.h>
#ifdef DEBUG
#include <stdio.h>
#endif

static inline uint8_t cpu_read(CPU *c, uint16_t addr) { return bus_cpu_read(c->bus, addr); }
static inline void cpu_write(CPU *c, uint16_t addr, uint8_t v) { bus_cpu_write(c->bus, addr, v); }

static inline void push(CPU *c, uint8_t v) { cpu_write(c, (uint16_t)(0x0100 | c->S), v); c->S--; }
static inline uint8_t pull(CPU *c) { c->S++; return cpu_read(c, (uint16_t)(0x0100 | c->S)); }

static inline void set_zn(CPU *c, uint8_t v) {
    set_bit_u8(&c->P, FLAG_Z, v == 0);
    set_bit_u8(&c->P, FLAG_N, (v & 0x80) != 0);
}

static inline uint16_t fetch16(CPU *c) {
    uint8_t lo = cpu_read(c, c->PC++);
    uint8_t hi = cpu_read(c, c->PC++);
    return make16(lo, hi);
}

void cpu_connect_bus(CPU *c, Bus *b) {
    c->bus = b;
}

void cpu_power_on(CPU *c) {
    memset(c, 0, sizeof(*c));
    c->P = FLAG_U | FLAG_I; // U set, IRQ disabled on power
    c->S = 0xFD;
    c->cycles = 0;
}

void cpu_reset(CPU *c) {
    c->P |= FLAG_I; // disable IRQ
    c->S -= 3; // emulate reset stack effect
    uint8_t lo = cpu_read(c, 0xFFFC);
    uint8_t hi = cpu_read(c, 0xFFFD);
    c->PC = make16(lo, hi);
    #ifdef DEBUG
    fprintf(stderr, "cpu_reset: vector=%02X%02X -> PC=%04X\n", hi, lo, c->PC);
    #endif
}

void cpu_irq(CPU *c) {
    if (!(c->P & FLAG_I)) {
        push(c, (uint8_t)((c->PC >> 8) & 0xFF));
        push(c, (uint8_t)(c->PC & 0xFF));
        uint8_t p = c->P & (uint8_t)~FLAG_B; p |= FLAG_U; push(c, p);
        c->P |= FLAG_I;
        uint16_t lo = cpu_read(c, 0xFFFE);
        uint16_t hi = cpu_read(c, 0xFFFF);
        c->PC = make16(lo, hi);
        c->cycles += 7;
    }
}

void cpu_nmi(CPU *c) {
    push(c, (uint8_t)((c->PC >> 8) & 0xFF));
    push(c, (uint8_t)(c->PC & 0xFF));
    uint8_t p = c->P & (uint8_t)~FLAG_B; p |= FLAG_U; push(c, p);
    c->P |= FLAG_I;
    uint16_t lo = cpu_read(c, 0xFFFA);
    uint16_t hi = cpu_read(c, 0xFFFB);
    c->PC = make16(lo, hi);
    c->cycles += 7;
}

// Addressing helpers
typedef struct { uint16_t addr; uint8_t extra_cycles; } Addr;

static inline Addr addr_imm(CPU *c) { Addr a = { c->PC++, 0 }; return a; }
static inline Addr addr_zp0(CPU *c) { Addr a = { cpu_read(c, c->PC++), 0 }; return a; }
static inline Addr addr_zpx(CPU *c) { uint8_t zp = cpu_read(c, c->PC++); Addr a = { (uint8_t)(zp + c->X), 0 }; return a; }
static inline Addr addr_zpy(CPU *c) { uint8_t zp = cpu_read(c, c->PC++); Addr a = { (uint8_t)(zp + c->Y), 0 }; return a; }
static inline Addr addr_abs(CPU *c) { Addr a = { fetch16(c), 0 }; return a; }
static inline Addr addr_abx(CPU *c) {
    uint16_t base = fetch16(c);
    uint16_t addr = (uint16_t)(base + c->X);
    Addr a = { addr, (uint8_t)(page_crossed(base, addr) ? 1 : 0) };
    return a;
}
static inline Addr addr_aby(CPU *c) {
    uint16_t base = fetch16(c);
    uint16_t addr = (uint16_t)(base + c->Y);
    Addr a = { addr, (uint8_t)(page_crossed(base, addr) ? 1 : 0) };
    return a;
}
static inline Addr addr_ind(CPU *c) { // JMP indirect with 6502 bug
    uint16_t ptr = fetch16(c);
    uint16_t lo_addr = ptr;
    uint16_t hi_addr = (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0x00FF));
    uint8_t lo = cpu_read(c, lo_addr);
    uint8_t hi = cpu_read(c, hi_addr);
    Addr a = { make16(lo, hi), 0 }; return a;
}
static inline Addr addr_izx(CPU *c) {
    uint8_t t = (uint8_t)(cpu_read(c, c->PC++) + c->X);
    uint8_t lo = cpu_read(c, t);
    uint8_t hi = cpu_read(c, (uint8_t)(t + 1));
    Addr a = { make16(lo, hi), 0 }; return a;
}
static inline Addr addr_izy(CPU *c) {
    uint8_t t = cpu_read(c, c->PC++);
    uint8_t lo = cpu_read(c, t);
    uint8_t hi = cpu_read(c, (uint8_t)(t + 1));
    uint16_t base = make16(lo, hi);
    uint16_t addr = (uint16_t)(base + c->Y);
    Addr a = { addr, (uint8_t)(page_crossed(base, addr) ? 1 : 0) }; return a;
}

// Branch helper
static inline uint16_t rel_addr(CPU *c) {
    int8_t off = (int8_t)cpu_read(c, c->PC++);
    return (uint16_t)(c->PC + off);
}

// ADC/SBC helpers (no decimal mode on NES)
static inline uint8_t adc(CPU *c, uint8_t a, uint8_t b) {
    uint16_t sum = (uint16_t)a + (uint16_t)b + (uint16_t)(c->P & FLAG_C ? 1 : 0);
    uint8_t res = (uint8_t)sum;
    set_bit_u8(&c->P, FLAG_C, sum > 0xFF);
    set_bit_u8(&c->P, FLAG_V, (~(a ^ b) & (a ^ res) & 0x80) != 0);
    set_zn(c, res);
    return res;
}
static inline uint8_t sbc(CPU *c, uint8_t a, uint8_t b) {
    // a + (~b) + C
    uint16_t diff = (uint16_t)a + (uint16_t)(~b) + (uint16_t)(c->P & FLAG_C ? 1 : 0);
    uint8_t res = (uint8_t)diff;
    set_bit_u8(&c->P, FLAG_C, diff & 0x100);
    set_bit_u8(&c->P, FLAG_V, ((a ^ b) & (a ^ res) & 0x80) != 0);
    set_zn(c, res);
    return res;
}

// Execute instruction
int cpu_step(CPU *c) {
    // Service pending interrupts (simplified; caller can trigger via cpu_irq/cpu_nmi)
    if (c->nmi_line) { c->nmi_line = false; cpu_nmi(c); return 7; }
    if (c->irq_line && !(c->P & FLAG_I)) { c->irq_line = false; cpu_irq(c); return 7; }

    uint8_t op = cpu_read(c, c->PC++);
    int cycles = 0;
    switch (op) {
        // BRK
        case 0x00: {
            c->PC++;
            push(c, (uint8_t)((c->PC >> 8) & 0xFF));
            push(c, (uint8_t)(c->PC & 0xFF));
            uint8_t p = c->P | FLAG_B | FLAG_U; push(c, p);
            c->P |= FLAG_I;
            uint16_t lo = cpu_read(c, 0xFFFE); uint16_t hi = cpu_read(c, 0xFFFF);
            c->PC = make16(lo, hi);
            cycles = 7;
            break;
        }

        // ORA
        case 0x09: { Addr a = addr_imm(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 2; break; }
        case 0x05: { Addr a = addr_zp0(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 3; break; }
        case 0x15: { Addr a = addr_zpx(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4; break; }
        case 0x0D: { Addr a = addr_abs(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4; break; }
        case 0x1D: { Addr a = addr_abx(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4 + a.extra_cycles; break; }
        case 0x19: { Addr a = addr_aby(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4 + a.extra_cycles; break; }
        case 0x01: { Addr a = addr_izx(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 6; break; }
        case 0x11: { Addr a = addr_izy(c); c->A |= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 5 + a.extra_cycles; break; }

        // AND
        case 0x29: { Addr a = addr_imm(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 2; break; }
        case 0x25: { Addr a = addr_zp0(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 3; break; }
        case 0x35: { Addr a = addr_zpx(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4; break; }
        case 0x2D: { Addr a = addr_abs(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4; break; }
        case 0x3D: { Addr a = addr_abx(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4 + a.extra_cycles; break; }
        case 0x39: { Addr a = addr_aby(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4 + a.extra_cycles; break; }
        case 0x21: { Addr a = addr_izx(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 6; break; }
        case 0x31: { Addr a = addr_izy(c); c->A &= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 5 + a.extra_cycles; break; }

        // EOR
        case 0x49: { Addr a = addr_imm(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 2; break; }
        case 0x45: { Addr a = addr_zp0(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 3; break; }
        case 0x55: { Addr a = addr_zpx(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4; break; }
        case 0x4D: { Addr a = addr_abs(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4; break; }
        case 0x5D: { Addr a = addr_abx(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4 + a.extra_cycles; break; }
        case 0x59: { Addr a = addr_aby(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 4 + a.extra_cycles; break; }
        case 0x41: { Addr a = addr_izx(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 6; break; }
        case 0x51: { Addr a = addr_izy(c); c->A ^= cpu_read(c, a.addr); set_zn(c, c->A); cycles = 5 + a.extra_cycles; break; }

        // ADC
        case 0x69: { Addr a = addr_imm(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 2; break; }
        case 0x65: { Addr a = addr_zp0(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 3; break; }
        case 0x75: { Addr a = addr_zpx(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 4; break; }
        case 0x6D: { Addr a = addr_abs(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 4; break; }
        case 0x7D: { Addr a = addr_abx(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 4 + a.extra_cycles; break; }
        case 0x79: { Addr a = addr_aby(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 4 + a.extra_cycles; break; }
        case 0x61: { Addr a = addr_izx(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 6; break; }
        case 0x71: { Addr a = addr_izy(c); c->A = adc(c, c->A, cpu_read(c, a.addr)); cycles = 5 + a.extra_cycles; break; }

        // SBC
        case 0xE9: { Addr a = addr_imm(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 2; break; }
        case 0xE5: { Addr a = addr_zp0(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 3; break; }
        case 0xF5: { Addr a = addr_zpx(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 4; break; }
        case 0xED: { Addr a = addr_abs(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 4; break; }
        case 0xFD: { Addr a = addr_abx(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 4 + a.extra_cycles; break; }
        case 0xF9: { Addr a = addr_aby(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 4 + a.extra_cycles; break; }
        case 0xE1: { Addr a = addr_izx(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 6; break; }
        case 0xF1: { Addr a = addr_izy(c); c->A = sbc(c, c->A, cpu_read(c, a.addr)); cycles = 5 + a.extra_cycles; break; }

        // CMP
        case 0xC9: { Addr a = addr_imm(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=2; break; }
        case 0xC5: { Addr a = addr_zp0(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=3; break; }
        case 0xD5: { Addr a = addr_zpx(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=4; break; }
        case 0xCD: { Addr a = addr_abs(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=4; break; }
        case 0xDD: { Addr a = addr_abx(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=4 + a.extra_cycles; break; }
        case 0xD9: { Addr a = addr_aby(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=4 + a.extra_cycles; break; }
        case 0xC1: { Addr a = addr_izx(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=6; break; }
        case 0xD1: { Addr a = addr_izy(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->A - v; set_bit_u8(&c->P,FLAG_C,c->A>=v); set_zn(c,(uint8_t)t); cycles=5 + a.extra_cycles; break; }

        // CPX
        case 0xE0: { Addr a=addr_imm(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->X-v; set_bit_u8(&c->P,FLAG_C,c->X>=v); set_zn(c,(uint8_t)t); cycles=2; break; }
        case 0xE4: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->X-v; set_bit_u8(&c->P,FLAG_C,c->X>=v); set_zn(c,(uint8_t)t); cycles=3; break; }
        case 0xEC: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->X-v; set_bit_u8(&c->P,FLAG_C,c->X>=v); set_zn(c,(uint8_t)t); cycles=4; break; }

        // CPY
        case 0xC0: { Addr a=addr_imm(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->Y-v; set_bit_u8(&c->P,FLAG_C,c->Y>=v); set_zn(c,(uint8_t)t); cycles=2; break; }
        case 0xC4: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->Y-v; set_bit_u8(&c->P,FLAG_C,c->Y>=v); set_zn(c,(uint8_t)t); cycles=3; break; }
        case 0xCC: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr); uint16_t t=(uint16_t)c->Y-v; set_bit_u8(&c->P,FLAG_C,c->Y>=v); set_zn(c,(uint8_t)t); cycles=4; break; }

        // INC
        case 0xE6: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr)+1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=5; break; }
        case 0xF6: { Addr a=addr_zpx(c); uint8_t v=cpu_read(c,a.addr)+1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0xEE: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr)+1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0xFE: { Addr a=addr_abx(c); uint8_t v=cpu_read(c,a.addr)+1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=7; break; }

        // INX, INY
        case 0xE8: { c->X++; set_zn(c,c->X); cycles=2; break; }
        case 0xC8: { c->Y++; set_zn(c,c->Y); cycles=2; break; }

        // DEC
        case 0xC6: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr)-1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=5; break; }
        case 0xD6: { Addr a=addr_zpx(c); uint8_t v=cpu_read(c,a.addr)-1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0xCE: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr)-1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0xDE: { Addr a=addr_abx(c); uint8_t v=cpu_read(c,a.addr)-1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=7; break; }

        // DEX, DEY
        case 0xCA: { c->X--; set_zn(c,c->X); cycles=2; break; }
        case 0x88: { c->Y--; set_zn(c,c->Y); cycles=2; break; }

        // ASL
        case 0x0A: { // Accumulator
            set_bit_u8(&c->P, FLAG_C, (c->A & 0x80) != 0);
            c->A <<= 1; set_zn(c,c->A); cycles=2; break;
        }
        case 0x06: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v<<=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=5; break; }
        case 0x16: { Addr a=addr_zpx(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v<<=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x0E: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v<<=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x1E: { Addr a=addr_abx(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v<<=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=7; break; }

        // LSR
        case 0x4A: { set_bit_u8(&c->P,FLAG_C,(c->A&1)!=0); c->A >>= 1; set_zn(c,c->A); cycles=2; break; }
        case 0x46: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v>>=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=5; break; }
        case 0x56: { Addr a=addr_zpx(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v>>=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x4E: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v>>=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x5E: { Addr a=addr_abx(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v>>=1; cpu_write(c,a.addr,v); set_zn(c,v); cycles=7; break; }

        // ROL
        case 0x2A: { uint8_t carry = (c->P & FLAG_C)?1:0; uint8_t newc = (c->A & 0x80)?1:0; c->A=(uint8_t)((c->A<<1)|carry); set_bit_u8(&c->P,FLAG_C,newc); set_zn(c,c->A); cycles=2; break; }
        case 0x26: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v=(uint8_t)((v<<1)|carry); cpu_write(c,a.addr,v); set_zn(c,v); cycles=5; break; }
        case 0x36: { Addr a=addr_zpx(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v=(uint8_t)((v<<1)|carry); cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x2E: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v=(uint8_t)((v<<1)|carry); cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x3E: { Addr a=addr_abx(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&0x80)!=0); v=(uint8_t)((v<<1)|carry); cpu_write(c,a.addr,v); set_zn(c,v); cycles=7; break; }

        // ROR
        case 0x6A: { uint8_t carry=(c->P&FLAG_C)?1:0; uint8_t newc=c->A&1; c->A=(uint8_t)((c->A>>1)|(carry<<7)); set_bit_u8(&c->P,FLAG_C,newc); set_zn(c,c->A); cycles=2; break; }
        case 0x66: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v=(uint8_t)((v>>1)|(carry<<7)); cpu_write(c,a.addr,v); set_zn(c,v); cycles=5; break; }
        case 0x76: { Addr a=addr_zpx(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v=(uint8_t)((v>>1)|(carry<<7)); cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x6E: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v=(uint8_t)((v>>1)|(carry<<7)); cpu_write(c,a.addr,v); set_zn(c,v); cycles=6; break; }
        case 0x7E: { Addr a=addr_abx(c); uint8_t v=cpu_read(c,a.addr); uint8_t carry=(c->P&FLAG_C)?1:0; set_bit_u8(&c->P,FLAG_C,(v&1)!=0); v=(uint8_t)((v>>1)|(carry<<7)); cpu_write(c,a.addr,v); set_zn(c,v); cycles=7; break; }

        // BIT
        case 0x24: { Addr a=addr_zp0(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_Z,(c->A & v)==0); set_bit_u8(&c->P,FLAG_V,(v&0x40)!=0); set_bit_u8(&c->P,FLAG_N,(v&0x80)!=0); cycles=3; break; }
        case 0x2C: { Addr a=addr_abs(c); uint8_t v=cpu_read(c,a.addr); set_bit_u8(&c->P,FLAG_Z,(c->A & v)==0); set_bit_u8(&c->P,FLAG_V,(v&0x40)!=0); set_bit_u8(&c->P,FLAG_N,(v&0x80)!=0); cycles=4; break; }

        // JMP
        case 0x4C: { Addr a=addr_abs(c); c->PC=a.addr; cycles=3; break; }
        case 0x6C: { Addr a=addr_ind(c); c->PC=a.addr; cycles=5; break; }

        // JSR, RTS, RTI
        case 0x20: { uint16_t a=fetch16(c); uint16_t ret=(uint16_t)(c->PC-1); push(c,hi8(ret)); push(c,lo8(ret)); c->PC=a; cycles=6; break; }
        case 0x60: { uint8_t lo=pull(c); uint8_t hi=pull(c); c->PC=(uint16_t)(make16(lo,hi)+1); cycles=6; break; }
        case 0x40: { uint8_t p=pull(c); uint8_t lo=pull(c); uint8_t hi=pull(c); c->P = (uint8_t)((p | FLAG_U) & ~FLAG_B); c->PC=make16(lo,hi); cycles=6; break; }

        // LDA
        case 0xA9: { Addr a=addr_imm(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=2; break; }
        case 0xA5: { Addr a=addr_zp0(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=3; break; }
        case 0xB5: { Addr a=addr_zpx(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=4; break; }
        case 0xAD: { Addr a=addr_abs(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=4; break; }
        case 0xBD: { Addr a=addr_abx(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=4 + a.extra_cycles; break; }
        case 0xB9: { Addr a=addr_aby(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=4 + a.extra_cycles; break; }
        case 0xA1: { Addr a=addr_izx(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=6; break; }
        case 0xB1: { Addr a=addr_izy(c); c->A=cpu_read(c,a.addr); set_zn(c,c->A); cycles=5 + a.extra_cycles; break; }

        // LDX
        case 0xA2: { Addr a=addr_imm(c); c->X=cpu_read(c,a.addr); set_zn(c,c->X); cycles=2; break; }
        case 0xA6: { Addr a=addr_zp0(c); c->X=cpu_read(c,a.addr); set_zn(c,c->X); cycles=3; break; }
        case 0xB6: { Addr a=addr_zpy(c); c->X=cpu_read(c,a.addr); set_zn(c,c->X); cycles=4; break; }
        case 0xAE: { Addr a=addr_abs(c); c->X=cpu_read(c,a.addr); set_zn(c,c->X); cycles=4; break; }
        case 0xBE: { Addr a=addr_aby(c); c->X=cpu_read(c,a.addr); set_zn(c,c->X); cycles=4 + a.extra_cycles; break; }

        // LDY
        case 0xA0: { Addr a=addr_imm(c); c->Y=cpu_read(c,a.addr); set_zn(c,c->Y); cycles=2; break; }
        case 0xA4: { Addr a=addr_zp0(c); c->Y=cpu_read(c,a.addr); set_zn(c,c->Y); cycles=3; break; }
        case 0xB4: { Addr a=addr_zpx(c); c->Y=cpu_read(c,a.addr); set_zn(c,c->Y); cycles=4; break; }
        case 0xAC: { Addr a=addr_abs(c); c->Y=cpu_read(c,a.addr); set_zn(c,c->Y); cycles=4; break; }
        case 0xBC: { Addr a=addr_abx(c); c->Y=cpu_read(c,a.addr); set_zn(c,c->Y); cycles=4 + a.extra_cycles; break; }

        // STA
        case 0x85: { Addr a=addr_zp0(c); cpu_write(c,a.addr,c->A); cycles=3; break; }
        case 0x95: { Addr a=addr_zpx(c); cpu_write(c,a.addr,c->A); cycles=4; break; }
        case 0x8D: { Addr a=addr_abs(c); cpu_write(c,a.addr,c->A); cycles=4; break; }
        case 0x9D: { Addr a=addr_abx(c); cpu_write(c,a.addr,c->A); cycles=5; break; }
        case 0x99: { Addr a=addr_aby(c); cpu_write(c,a.addr,c->A); cycles=5; break; }
        case 0x81: { Addr a=addr_izx(c); cpu_write(c,a.addr,c->A); cycles=6; break; }
        case 0x91: { Addr a=addr_izy(c); cpu_write(c,a.addr,c->A); cycles=6; break; }

        // STX
        case 0x86: { Addr a=addr_zp0(c); cpu_write(c,a.addr,c->X); cycles=3; break; }
        case 0x96: { Addr a=addr_zpy(c); cpu_write(c,a.addr,c->X); cycles=4; break; }
        case 0x8E: { Addr a=addr_abs(c); cpu_write(c,a.addr,c->X); cycles=4; break; }

        // STY
        case 0x84: { Addr a=addr_zp0(c); cpu_write(c,a.addr,c->Y); cycles=3; break; }
        case 0x94: { Addr a=addr_zpx(c); cpu_write(c,a.addr,c->Y); cycles=4; break; }
        case 0x8C: { Addr a=addr_abs(c); cpu_write(c,a.addr,c->Y); cycles=4; break; }

        // TAX, TAY, TSX, TXA, TXS, TYA
        case 0xAA: { c->X=c->A; set_zn(c,c->X); cycles=2; break; }
        case 0xA8: { c->Y=c->A; set_zn(c,c->Y); cycles=2; break; }
        case 0xBA: { c->X=c->S; set_zn(c,c->X); cycles=2; break; }
        case 0x8A: { c->A=c->X; set_zn(c,c->A); cycles=2; break; }
        case 0x9A: { c->S=c->X; cycles=2; break; }
        case 0x98: { c->A=c->Y; set_zn(c,c->A); cycles=2; break; }

        // PLA, PLP, PHA, PHP
        case 0x68: { c->A = pull(c); set_zn(c,c->A); cycles=4; break; }
        case 0x28: { c->P = (uint8_t)((pull(c) | FLAG_U) & ~FLAG_B); cycles=4; break; }
        case 0x48: { push(c,c->A); cycles=3; break; }
        case 0x08: { push(c,(uint8_t)(c->P | FLAG_B | FLAG_U)); cycles=3; break; }

        // CLC, CLD, CLI, CLV, SEC, SED, SEI
        case 0x18: { c->P &= (uint8_t)~FLAG_C; cycles=2; break; }
        case 0xD8: { c->P &= (uint8_t)~FLAG_D; cycles=2; break; }
        case 0x58: { c->P &= (uint8_t)~FLAG_I; cycles=2; break; }
        case 0xB8: { c->P &= (uint8_t)~FLAG_V; cycles=2; break; }
        case 0x38: { c->P |= FLAG_C; cycles=2; break; }
        case 0xF8: { c->P |= FLAG_D; cycles=2; break; }
        case 0x78: { c->P |= FLAG_I; cycles=2; break; }

        // NOP
        case 0xEA: { cycles=2; break; }

        // BCC, BCS, BEQ, BMI, BNE, BPL, BVC, BVS
        case 0x90: { uint16_t t=rel_addr(c); if(!(c->P&FLAG_C)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }
        case 0xB0: { uint16_t t=rel_addr(c); if( (c->P&FLAG_C)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }
        case 0xF0: { uint16_t t=rel_addr(c); if( (c->P&FLAG_Z)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }
        case 0x30: { uint16_t t=rel_addr(c); if( (c->P&FLAG_N)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }
        case 0xD0: { uint16_t t=rel_addr(c); if(!(c->P&FLAG_Z)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }
        case 0x10: { uint16_t t=rel_addr(c); if(!(c->P&FLAG_N)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }
        case 0x50: { uint16_t t=rel_addr(c); if(!(c->P&FLAG_V)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }
        case 0x70: { uint16_t t=rel_addr(c); if( (c->P&FLAG_V)){ if(page_crossed(c->PC,t)) cycles+=1; c->PC=t; cycles+=1;} cycles+=2; break; }

        // LSR/ASL RMW handled above; RMW opcodes covered

        // RMW INC/DEC done; ROL/ROR done

        default: {
            // Treat unknown/undocumented opcodes as NOP to remain forward-progressing
            // Many unofficial NOP variants exist; we map all others here
            cycles = 2;
            break;
        }
    }
    c->cycles += (uint64_t)cycles;
    return cycles;
}
