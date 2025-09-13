#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nes.h"
#include "video.h"
#ifdef HAVE_SDL2
#include <SDL.h>
#endif

static int parse_frames_arg(int argc, char **argv) {
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--frames") == 0) return atoi(argv[i+1]);
    }
    return -1;
}

static int parse_trace_ins(int argc, char **argv) {
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--trace-ins") == 0) return atoi(argv[i+1]);
    }
    return 0;
}

static int parse_trace_frames(int argc, char **argv) {
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--trace-frames") == 0) return atoi(argv[i+1]);
    }
    return 0;
}

static bool parse_use_sdl(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sdl") == 0) return true;
    }
    return false;
}

static int parse_fps(int argc, char **argv) {
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--fps") == 0) return atoi(argv[i+1]);
    }
    return 30;
}

static bool parse_debug_ppu(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug-ppu") == 0) return true;
    }
    return false;
}

static bool parse_bg_fallback(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bg-fallback") == 0) return true;
    }
    return false;
}

/* removed: per-scanline PPU trace parsing */

static bool parse_no_audio(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-audio") == 0) return true;
    }
    return false;
}

static const char *parse_str_opt(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], flag) == 0) return argv[i+1];
    }
    return NULL;
}

static void apply_config(Video *vid, int *fps, bool *no_audio, const char *path) {
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        // trim trailing spaces in key
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        // trim val newline
        size_t len = strlen(val);
        while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r')) val[--len] = '\0';
        if (strcmp(key, "fps") == 0) {
            int v = atoi(val); if (v > 0) *fps = v;
        } else if (strcmp(key, "audio") == 0) {
            if (strcmp(val, "off") == 0 || strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0) *no_audio = true;
            if (strcasecmp(val, "on") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0) *no_audio = false;
        } else if (vid && strcmp(key, "p1map") == 0) {
            if (!video_parse_and_set_keymap(vid, 1, val)) fprintf(stderr, "Warning: invalid p1map in config.\n");
        } else if (vid && strcmp(key, "p2map") == 0) {
            if (!video_parse_and_set_keymap(vid, 2, val)) fprintf(stderr, "Warning: invalid p2map in config.\n");
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom.nes> [--frames N] [--trace-ins N] [--trace-frames N] [--sdl] [--no-audio] [--fps N] [--p1map CSV] [--p2map CSV] [--config FILE] [--debug-ppu] [--bg-fallback]\n", argv[0]);
        return 1;
    }
    const char *rom_path = argv[1];
    int frames_to_run = parse_frames_arg(argc, argv);
    if (frames_to_run < 0) frames_to_run = 300; // default ~5s at 60fps
    int trace_ins = parse_trace_ins(argc, argv);
    int trace_frames = parse_trace_frames(argc, argv);
    bool want_sdl = parse_use_sdl(argc, argv);
    bool no_audio = parse_no_audio(argc, argv);
    int fps = parse_fps(argc, argv);
    if (fps <= 0) fps = 30;
    const char *p1map = parse_str_opt(argc, argv, "--p1map");
    const char *p2map = parse_str_opt(argc, argv, "--p2map");
    const char *cfg = parse_str_opt(argc, argv, "--config");
    bool debug_ppu = parse_debug_ppu(argc, argv);
    bool bg_fallback = parse_bg_fallback(argc, argv);

    NES nes;
    nes_init(&nes, !no_audio);
    if (debug_ppu) {
        ppu_set_debug(true);
    }
    int rc = nes_load_rom(&nes, rom_path);
    if (rc != 0) {
        fprintf(stderr, "Failed to load ROM '%s' (err %d). Only iNES mapper 0 is supported.\n", rom_path, rc);
        return 2;
    }
    nes_reset(&nes);

    // Optional instruction trace first
    if (trace_ins > 0) {
        printf("Tracing %d instructions...\n", trace_ins);
        for (int i = 0; i < trace_ins; ++i) {
            uint16_t pc = nes.cpu.PC;
            uint8_t op = bus_cpu_read(&nes.bus, pc);
            int used = nes_step_instruction(&nes);
            printf("ins %6d  PC:%04X OP:%02X  A:%02X X:%02X Y:%02X P:%02X S:%02X  cyc+%d\n",
                   i+1, pc, op, nes.cpu.A, nes.cpu.X, nes.cpu.Y, nes.cpu.P, nes.cpu.S, used);
        }
    }

    // Simple run loop: cycles per frame ≈ 29830
    const int cycles_per_frame = 29830;
    printf("Running %d frames...\n", frames_to_run);

    clock_t start = clock();
    // Optional SDL window
    Video *vid = NULL;
    bool have_window = false;
    if (want_sdl) {
        have_window = video_init(&vid, "NES-EMU", 256, 240, 3);
        if (!have_window) {
            printf("SDL2 not available; continuing headless.\n");
        }
        // Apply config file mappings and settings if provided
        if (cfg) apply_config(vid, &fps, &no_audio, cfg);
        if (have_window) {
            if (p1map) {
                if (!video_parse_and_set_keymap(vid, 1, p1map)) {
                    fprintf(stderr, "Warning: failed to parse --p1map, using defaults.\n");
                }
            }
            if (p2map) {
                if (!video_parse_and_set_keymap(vid, 2, p2map)) {
                    fprintf(stderr, "Warning: failed to parse --p2map, using defaults.\n");
                }
            }
        }
    }

    const double target_ms = 1000.0 / (double)fps; // FPS limiter
    for (int f = 0; f < frames_to_run; ++f) {
        #ifdef HAVE_SDL2
        uint32_t t0 = SDL_GetTicks();
        #endif
        nes_run_cycles(&nes, cycles_per_frame);

        if (trace_frames > 0 && f < trace_frames) {
            printf("frame %5d  PC:%04X  A:%02X X:%02X Y:%02X P:%02X S:%02X\n",
                   f+1, nes.cpu.PC, nes.cpu.A, nes.cpu.X, nes.cpu.Y, nes.cpu.P, nes.cpu.S);
        }

        if (have_window) {
            bool quit = false;
            uint8_t pad1 = 0, pad2 = 0;
            video_poll(vid, &quit, &pad1, &pad2);
            controller_set_state(&nes.ctrl1, pad1);
            controller_set_state(&nes.ctrl2, pad2);
            if (quit) break;
            if (bg_fallback) {
                const uint32_t *fb = ppu_render_frame(&nes.ppu);
                video_present(vid, fb);
            } else {
                // Present the framebuffer (already filled per-dot by PPU stepper)
                video_present(vid, nes.ppu.framebuffer);
            }
        }

        #ifdef HAVE_SDL2
        uint32_t elapsed = SDL_GetTicks() - t0;
        if (elapsed < (uint32_t)target_ms) {
            SDL_Delay((uint32_t)(target_ms - elapsed));
        }
        #endif
    }
    clock_t end = clock();
    double secs = (double)(end - start) / CLOCKS_PER_SEC;
    printf("Done. Ran %d frames in %.2f seconds.\n", frames_to_run, secs);
    (void)secs;

    if (nes.apu) apu_shutdown(&nes.apu);
    if (vid) video_shutdown(&vid);
    cartridge_free(&nes.cart);
    return 0;
}
