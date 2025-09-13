#include "apu.h"

#ifdef HAVE_SDL2
#include <SDL.h>
#include <math.h>
#include "bus.h"

typedef struct APU {
    SDL_AudioDeviceID dev;
    SDL_AudioSpec spec;
    // Pulse 1 state (ultra simplified)
    double phase;
    double phase_inc;
    float volume;
    bool enabled;
    uint16_t timer; // 11-bit
    // Triangle channel
    double tri_phase;
    double tri_inc;
    float tri_volume;
    bool tri_enabled;
    uint16_t tri_timer;
    // Noise channel
    double noise_phase;
    double noise_inc;
    float noise_volume;
    bool noise_enabled;
    uint16_t noise_period_index;
    uint16_t lfsr;
    bool noise_mode;

    // Frame sequencer
    int frame_mode; // 0=4-step, 1=5-step
    bool irq_inhibit;
    int cpu_cycle_mod; // cycles since last frame reset
    bool frame_irq;

    // Envelope and length (approximate)
    // Pulse 1 envelope
    bool p1_env_const; // $4000 bit4
    uint8_t p1_env_period; // lower 4 bits
    uint8_t p1_env_decay; // current decay (0..15)
    bool p1_env_loop; // bit5
    bool p1_env_start;
    uint8_t p1_env_div;
    uint8_t p1_length; // length counter
    // Triangle linear counter
    uint8_t tri_linear_reload; // $4008
    bool tri_control; // $4008 bit7 (also length counter halt)
    uint8_t tri_length;
    uint8_t tri_linear_counter;
    // Noise envelope/length
    bool noise_env_const; uint8_t noise_env_period; bool noise_env_loop; uint8_t noise_env_decay; uint8_t noise_length;
    bool noise_env_start; uint8_t noise_env_div;

    // DMC (basic)
    bool dmc_enabled;
    bool dmc_irq_enable;
    bool dmc_irq_flag;
    uint8_t dmc_rate_index;
    uint8_t dmc_output; // 0..127
    uint16_t dmc_sample_start; // base addr
    uint16_t dmc_sample_length; // length bytes
    uint16_t dmc_cur_addr;
    uint16_t dmc_remaining;
    uint8_t dmc_shift_reg;
    uint8_t dmc_bits_remaining;
    bool dmc_buffer_full;
    uint8_t dmc_buffer;
    double dmc_phase;
    double dmc_inc;

    // Bus for DMC fetches
    Bus *bus;
} APU;

static void apu_recalc_phase(APU *a) {
    if (!a->enabled || a->timer == 0x7FF) { a->phase_inc = 0.0; return; }
    double cpu = 1789773.0;
    double freq = cpu / (16.0 * (a->timer + 1));
    if (freq < 20.0) { a->phase_inc = 0.0; return; }
    a->phase_inc = freq / (double)a->spec.freq;
}

static void apu_recalc_triangle(APU *a) {
    double cpu = 1789773.0;
    if (!a->tri_enabled) { a->tri_inc = 0.0; return; }
    double freq = cpu / (32.0 * (a->tri_timer + 1));
    if (freq < 20.0) { a->tri_inc = 0.0; return; }
    a->tri_inc = freq / (double)a->spec.freq;
}

static const int NOISE_PERIODS[16] = {
    4, 8, 16, 32, 64, 96, 128, 160,
    202, 254, 380, 508, 762, 1016, 2034, 4068
};

static const int DMC_PERIODS[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106, 85,  72,  54
};

static void apu_recalc_noise(APU *a) {
    double cpu = 1789773.0;
    if (!a->noise_enabled) { a->noise_inc = 0.0; return; }
    int period = NOISE_PERIODS[a->noise_period_index & 0x0F];
    double freq = cpu / (double)period;
    if (freq < 20.0) { a->noise_inc = 0.0; return; }
    a->noise_inc = freq / (double)a->spec.freq;
}

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) {
    APU *a = (APU*)ud;
    int samples = len / (int)sizeof(float);
    float *out = (float*)stream;
    for (int i = 0; i < samples; ++i) {
        // Pulse amplitude 0..15
        float pulse_amp = 0.0f;
        if (a->phase_inc > 0.0 && a->p1_length > 0) {
            double ph = a->phase; a->phase += a->phase_inc; if (a->phase >= 1.0) a->phase -= 1.0;
            double duty = 0.125; // 12.5%
            float env = (float)(a->p1_env_const ? a->p1_env_period : a->p1_env_decay);
            pulse_amp = (ph < duty ? env : 0.0f);
        }

        // Triangle amplitude 0..15
        float tri_amp = 0.0f;
        if (a->tri_inc > 0.0 && a->tri_length > 0 && a->tri_linear_counter > 0) {
            double t = a->tri_phase; a->tri_phase += a->tri_inc; if (a->tri_phase >= 1.0) a->tri_phase -= 1.0;
            double tri = (t < 0.5 ? (t * 2.0) : (2.0 - t * 2.0));
            tri_amp = (float)(tri * 15.0);
        }

        // Noise amplitude 0..15
        float noise_amp = 0.0f;
        if (a->noise_inc > 0.0 && a->noise_length > 0) {
            a->noise_phase += a->noise_inc;
            if (a->noise_phase >= 1.0) {
                a->noise_phase -= 1.0;
                uint16_t tap = a->noise_mode ? 6 : 1;
                uint16_t bit = (uint16_t)(((a->lfsr ^ (a->lfsr >> tap)) & 1));
                a->lfsr = (uint16_t)((a->lfsr >> 1) | (bit << 14));
            }
            float env = (float)(a->noise_env_const ? a->noise_env_period : a->noise_env_decay);
            noise_amp = (a->lfsr & 1) ? env : 0.0f;
        }

        float dmc_amp = (float)a->dmc_output; // 0..127

        // NES mixing formulas
        float pulse_sum = pulse_amp; // only pulse 1
        float pulse_out = (pulse_sum <= 0.0f) ? 0.0f : (95.88f / (8128.0f / pulse_sum + 100.0f));
        float tnd_in = (tri_amp / 8227.0f) + (noise_amp / 12241.0f) + (dmc_amp / 22638.0f);
        float tnd_out = (tnd_in <= 0.0f) ? 0.0f : (159.79f / (1.0f / tnd_in + 100.0f));
        float s = pulse_out + tnd_out;
        if (s > 1.0f) s = 1.0f; else if (s < 0.0f) s = 0.0f;
        out[i] = (s * 2.0f - 1.0f);
    }
}

bool apu_init(APU **out) {
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            *out = NULL; return false;
        }
    }
    APU *a = (APU*)SDL_calloc(1, sizeof(APU));
    if (!a) { *out = NULL; return false; }
    a->volume = 0.1f;
    a->enabled = true;
    a->phase = 0.0;
    a->phase_inc = 0.0;
    a->timer = 0x7FF; // silent until configured
    a->tri_volume = 0.1f; a->tri_enabled = false; a->tri_timer = 0x7FF; a->tri_phase = 0.0; a->tri_inc = 0.0;
    a->noise_volume = 0.05f; a->noise_enabled = false; a->noise_period_index = 0; a->noise_phase = 0.0; a->noise_inc = 0.0; a->lfsr = 1; a->noise_mode = false;
    a->frame_mode = 0; a->irq_inhibit = true; a->cpu_cycle_mod = 0; a->frame_irq = false;
    a->p1_env_const = false; a->p1_env_period = 0; a->p1_env_decay = 15; a->p1_env_loop = false; a->p1_env_start = false; a->p1_env_div = 0; a->p1_length = 0;
    a->tri_linear_reload = 0; a->tri_control = false; a->tri_length = 0; a->tri_linear_counter = 0;
    a->noise_env_const = false; a->noise_env_period = 0; a->noise_env_loop = false; a->noise_env_decay = 15; a->noise_env_start = false; a->noise_env_div = 0; a->noise_length = 0;
    // DMC defaults
    a->dmc_enabled = false; a->dmc_irq_enable = false; a->dmc_irq_flag = false;
    a->dmc_rate_index = 0; a->dmc_output = 0x20; a->dmc_sample_start = 0xC000; a->dmc_sample_length = 1;
    a->dmc_cur_addr = 0; a->dmc_remaining = 0; a->dmc_shift_reg = 0; a->dmc_bits_remaining = 0;
    a->dmc_buffer_full = false; a->dmc_buffer = 0; a->dmc_phase = 0.0; a->dmc_inc = 0.0; a->bus = NULL;

    SDL_AudioSpec want = {0};
    want.freq = 44100;
    want.format = AUDIO_F32;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_cb;
    want.userdata = a;
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &a->spec, 0);
    if (!a->dev) { SDL_free(a); *out = NULL; return false; }
    SDL_PauseAudioDevice(a->dev, 0);
    *out = a;
    return true;
}

void apu_connect_bus(APU *a, Bus *b) {
    (void)a; (void)b;
    // Current simple implementation does not use bus; placeholder for DMC
}

void apu_shutdown(APU **pa) {
    if (!pa || !*pa) return;
    APU *a = *pa; *pa = NULL;
    if (a->dev) {
        SDL_PauseAudioDevice(a->dev, 1);
        SDL_CloseAudioDevice(a->dev);
    }
    SDL_free(a);
}

void apu_write(APU *a, uint16_t addr, uint8_t data) {
    if (!a) return;
    switch (addr) {
        case 0x4000: {
            // Duty ignored; envelope
            a->p1_env_const = (data & 0x10) != 0;
            a->p1_env_loop = (data & 0x20) != 0;
            a->p1_env_period = (uint8_t)(data & 0x0F);
            float v = (float)(a->p1_env_const ? a->p1_env_period : a->p1_env_decay) / 15.0f;
            a->volume = v * 0.2f;
            break;
        }
        case 0x4001: {
            // Sweep (ignored in audio generation; placeholder)
            (void)data; break;
        }
        case 0x4002: {
            a->timer = (uint16_t)((a->timer & 0x0700) | data);
            apu_recalc_phase(a);
            break;
        }
        case 0x4003: {
            a->timer = (uint16_t)(((data & 0x07) << 8) | (a->timer & 0x00FF));
            a->phase = 0.0; // restart
            // Length counter load from table
            static const uint8_t LENGTH_TABLE[32] = {
                10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
                12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
            };
            a->p1_length = LENGTH_TABLE[(data >> 3) & 0x1F];
            a->p1_env_start = true;
            apu_recalc_phase(a);
            break;
        }
        case 0x4008: { // Triangle linear counter (ignored); use low 4 bits as volume
            a->tri_control = (data & 0x80) != 0;
            a->tri_linear_reload = (uint8_t)(data & 0x7F);
            a->tri_volume = (float)(a->tri_linear_counter & 0x1F) / 31.0f * 0.2f;
            break;
        }
        case 0x400A: { // Triangle timer low
            a->tri_timer = (uint16_t)((a->tri_timer & 0x0700) | data);
            apu_recalc_triangle(a);
            break;
        }
        case 0x400B: { // Triangle timer high
            a->tri_timer = (uint16_t)(((data & 0x07) << 8) | (a->tri_timer & 0x00FF));
            a->tri_phase = 0.0;
            static const uint8_t LENGTH_TABLE[32] = {
                10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
                12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
            };
            a->tri_length = LENGTH_TABLE[(data >> 3) & 0x1F];
            a->tri_linear_counter = a->tri_linear_reload;
            apu_recalc_triangle(a);
            break;
        }
        case 0x400C: { // Noise volume
            a->noise_env_const = (data & 0x10) != 0;
            a->noise_env_loop = (data & 0x20) != 0;
            a->noise_env_period = (uint8_t)(data & 0x0F);
            float v = (float)(a->noise_env_const ? a->noise_env_period : a->noise_env_decay) / 15.0f;
            a->noise_volume = v * 0.2f;
            break;
        }
        case 0x400E: { // Noise period index
            a->noise_period_index = (uint16_t)(data & 0x0F);
            a->noise_mode = (data & 0x80) != 0;
            apu_recalc_noise(a);
            break;
        }
        case 0x400F: { // Noise length; also reset LFSR
            a->lfsr = 1;
            static const uint8_t LENGTH_TABLE[32] = {
                10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
                12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
            };
            a->noise_length = LENGTH_TABLE[(data >> 3) & 0x1F];
            a->noise_env_start = true;
            break;
        }
        case 0x4015: {
            a->enabled = (data & 0x01) != 0;
            a->tri_enabled = (data & 0x04) != 0;
            a->noise_enabled = (data & 0x08) != 0;
            bool dmc_en = (data & 0x10) != 0;
            if (dmc_en && !a->dmc_enabled) {
                a->dmc_enabled = true;
                if (a->dmc_remaining == 0) {
                    a->dmc_cur_addr = a->dmc_sample_start;
                    a->dmc_remaining = a->dmc_sample_length;
                }
            } else if (!dmc_en) {
                a->dmc_enabled = false;
                a->dmc_remaining = 0;
            }
            apu_recalc_phase(a);
            apu_recalc_triangle(a);
            apu_recalc_noise(a);
            break;
        }
        case 0x4010: { // DMC control
            a->dmc_irq_enable = (data & 0x80) != 0;
            a->dmc_rate_index = (uint8_t)(data & 0x0F);
            double cpu = 1789773.0; int period = DMC_PERIODS[a->dmc_rate_index];
            a->dmc_inc = (cpu / (double)period) / (double)a->spec.freq;
            break;
        }
        case 0x4011: { // DMC direct load
            a->dmc_output = (uint8_t)(data & 0x7F);
            break;
        }
        case 0x4012: { // sample address
            a->dmc_sample_start = (uint16_t)(0xC000 + ((uint16_t)data << 6));
            break;
        }
        case 0x4013: { // sample length
            a->dmc_sample_length = (uint16_t)(((uint16_t)data << 4) + 1);
            break;
        }
        case 0x4017: {
            // Frame counter
            a->frame_mode = (data & 0x80) ? 1 : 0;
            a->irq_inhibit = (data & 0x40) != 0;
            a->cpu_cycle_mod = 0; // reset sequence timing
            break;
        }
        default:
            break;
    }
}

uint8_t apu_read(APU *a, uint16_t addr) {
    if (!a) return 0;
    if (addr == 0x4015) {
        uint8_t st = 0;
        if (a->enabled && a->p1_length) st |= 0x01;
        if (a->tri_enabled && a->tri_length) st |= 0x04;
        if (a->noise_enabled && a->noise_length) st |= 0x08;
        if (a->dmc_enabled && a->dmc_remaining) st |= 0x10;
        if (a->frame_irq) st |= 0x40;
        if (a->dmc_irq_flag) st |= 0x80;
        // Clear frame IRQ and DMC IRQ on read
        a->frame_irq = false;
        a->dmc_irq_flag = false;
        return st;
    }
    return 0;
}

static void apu_quarter_frame(APU *a) {
    // Envelope tick (approx): decay towards 0, loop if set
    // Pulse 1 envelope
    if (a->p1_env_start) {
        a->p1_env_start = false;
        a->p1_env_decay = 15;
        a->p1_env_div = a->p1_env_period;
    } else {
        if (a->p1_env_div > 0) a->p1_env_div--; else {
            a->p1_env_div = a->p1_env_period;
            if (a->p1_env_decay > 0) a->p1_env_decay--; else if (a->p1_env_loop) a->p1_env_decay = 15;
        }
    }
    a->volume = ((float)(a->p1_env_const ? a->p1_env_period : a->p1_env_decay) / 15.0f) * 0.2f;
    // Noise envelope
    if (a->noise_env_start) {
        a->noise_env_start = false;
        a->noise_env_decay = 15;
        a->noise_env_div = a->noise_env_period;
    } else {
        if (a->noise_env_div > 0) a->noise_env_div--; else {
            a->noise_env_div = a->noise_env_period;
            if (a->noise_env_decay > 0) a->noise_env_decay--; else if (a->noise_env_loop) a->noise_env_decay = 15;
        }
    }
    a->noise_volume = ((float)(a->noise_env_const ? a->noise_env_period : a->noise_env_decay) / 15.0f) * 0.2f;
    // Triangle linear counter reload if control flag set
    if (a->tri_control) {
        a->tri_linear_counter = a->tri_linear_reload;
    } else if (a->tri_linear_counter > 0) {
        a->tri_linear_counter--;
    }
}

static void apu_half_frame(APU *a) {
    // Length counters
    if (!a->p1_env_loop && a->p1_length > 0) a->p1_length--;
    if (!a->tri_control && a->tri_length > 0) a->tri_length--;
    if (!a->noise_env_loop && a->noise_length > 0) a->noise_length--;
}

void apu_tick_cpu_cycles(APU *a, int cpu_cycles) {
    if (!a) return;
    // 4-step sequence clocks at 3729, 7457, 11186, 14916 CPU cycles
    static const int step_times[4] = {3729, 7457, 11186, 14916};
    a->cpu_cycle_mod += cpu_cycles;
    while (1) {
        int idx = 0;
        while (idx < 4 && a->cpu_cycle_mod >= step_times[idx]) idx++;
        if (idx == 0) break;
        // Consume whole sequence period if exceeded end
        if (a->cpu_cycle_mod >= step_times[3]) {
            a->cpu_cycle_mod -= step_times[3];
            // Step 3: quarter + half
            apu_quarter_frame(a);
            apu_half_frame(a);
            if (!a->irq_inhibit && a->frame_mode == 0) a->frame_irq = true;
        } else {
            // Handle intermediate steps up to idx
            for (int i = 0; i < idx; ++i) {
                apu_quarter_frame(a);
                if (i == 1 || i == 3) apu_half_frame(a);
            }
            a->cpu_cycle_mod = 0;
        }
        // In 5-step mode, behavior differs; we approximate to 4-step without IRQ
        if (a->frame_mode == 1) {
            // No IRQ; handled via irq_inhibit flag
        }
    }

    // DMC stepping: process bits at DMC rate
    if (a->dmc_enabled && a->dmc_inc > 0.0) {
        a->dmc_phase += a->dmc_inc * (double)cpu_cycles;
        while (a->dmc_phase >= 1.0) {
            a->dmc_phase -= 1.0;
            if (a->dmc_bits_remaining == 0) {
                if (!a->dmc_buffer_full && a->bus && a->dmc_remaining > 0) {
                    a->dmc_buffer = bus_cpu_read(a->bus, a->dmc_cur_addr);
                    a->dmc_buffer_full = true;
                    a->dmc_cur_addr++;
                    if (a->dmc_cur_addr == 0x0000) a->dmc_cur_addr = 0x8000; // wrap
                    a->dmc_remaining--;
                    if (a->dmc_remaining == 0 && a->dmc_irq_enable) a->dmc_irq_flag = true;
                }
                if (a->dmc_buffer_full) {
                    a->dmc_shift_reg = a->dmc_buffer;
                    a->dmc_bits_remaining = 8;
                    a->dmc_buffer_full = false;
                } else {
                    // No sample data: hold shift reg at 1s
                    a->dmc_shift_reg = 0xFF;
                    a->dmc_bits_remaining = 8;
                }
            }
            // Output bit adjusts 7-bit output towards max/min by 2
            if (a->dmc_shift_reg & 1) {
                if (a->dmc_output <= 125) a->dmc_output += 2;
            } else {
                if (a->dmc_output >= 2) a->dmc_output -= 2;
            }
            a->dmc_shift_reg >>= 1;
            if (a->dmc_bits_remaining) a->dmc_bits_remaining--;
        }
    }
}

#include <stdbool.h>
bool apu_frame_irq_pending(APU *a) { return a ? a->frame_irq : false; }
bool apu_dmc_irq_pending(APU *a) { return a ? a->dmc_irq_flag : false; }

#else

// Headless stubs without SDL2
struct APU { int dummy; };
bool apu_init(APU **out) { *out = NULL; return false; }
void apu_shutdown(APU **out) { (void)out; }
void apu_write(APU *a, uint16_t addr, uint8_t data) { (void)a; (void)addr; (void)data; }

#endif
