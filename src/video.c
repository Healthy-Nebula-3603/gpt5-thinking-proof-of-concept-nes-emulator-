#include "video.h"

#ifdef HAVE_SDL2
#include <SDL.h>
#include <string.h>

struct Video {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int w, h;
    // Overscan crop in source pixels (NES space)
    int crop_l, crop_r, crop_t, crop_b;
    uint8_t pad1_state;
    uint8_t pad2_state;
    SDL_Keycode map1[8];
    SDL_Keycode map2[8];
};

bool video_init(Video **out, const char *title, int width, int height, int scale) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }
    int w = width * (scale > 0 ? scale : 1);
    int h = height * (scale > 0 ? scale : 1);
    SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, 0);
    if (!win) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);
    if (!tex) {
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return false;
    }
    Video *v = (Video*)SDL_calloc(1, sizeof(Video));
    v->win = win; v->ren = ren; v->w = w; v->h = h;
    v->tex = tex;
    // Default: hide 8 px on left/right to emulate CRT overscan
    v->crop_l = 8; v->crop_r = 8; v->crop_t = 0; v->crop_b = 0;
    // Defaults for P1: Z,X,RShift,Return,Up,Down,Left,Right
    SDL_Keycode def1[8] = { SDLK_z, SDLK_x, SDLK_RSHIFT, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT };
    memcpy(v->map1, def1, sizeof(def1));
    // Defaults for P2: N,M,LShift,RCTRL,I,K,J,L
    SDL_Keycode def2[8] = { SDLK_n, SDLK_m, SDLK_LSHIFT, SDLK_RCTRL, SDLK_i, SDLK_k, SDLK_j, SDLK_l };
    memcpy(v->map2, def2, sizeof(def2));
    *out = v;
    return true;
}

void video_fill(Video *v, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(v->ren, r, g, b, 255);
    SDL_RenderClear(v->ren);
    SDL_RenderPresent(v->ren);
}

void video_poll(Video *v, bool *quit, uint8_t *pad1_state, uint8_t *pad2_state) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { *quit = true; }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) { *quit = true; }
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            bool down = (e.type == SDL_KEYDOWN);
            SDL_Keycode k = e.key.keysym.sym;
            uint8_t b = 0;
            int pad = 0; // 1 or 2
            for (int i = 0; i < 8; ++i) { if (k == v->map1[i]) { b = (uint8_t)i; pad = 1; break; } }
            if (pad == 0) { for (int i = 0; i < 8; ++i) { if (k == v->map2[i]) { b = (uint8_t)i; pad = 2; break; } } }
            if (pad == 0) b = 255;
            if (b != 255) {
                uint8_t mask = (uint8_t)(1u << b);
                if (pad == 1) {
                    if (down) v->pad1_state |= mask; else v->pad1_state &= (uint8_t)~mask;
                } else if (pad == 2) {
                    if (down) v->pad2_state |= mask; else v->pad2_state &= (uint8_t)~mask;
                }
            }
        }
    }
    if (pad1_state) *pad1_state = v->pad1_state;
    if (pad2_state) *pad2_state = v->pad2_state;
}

void video_shutdown(Video **pv) {
    if (!pv || !*pv) return;
    Video *v = *pv; *pv = NULL;
    if (v->tex) SDL_DestroyTexture(v->tex);
    if (v->ren) SDL_DestroyRenderer(v->ren);
    if (v->win) SDL_DestroyWindow(v->win);
    SDL_free(v);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void video_present(Video *v, const uint32_t *pixels) {
    if (!v || !v->tex) return;
    SDL_UpdateTexture(v->tex, NULL, pixels, 256 * sizeof(uint32_t));
    SDL_RenderClear(v->ren);
    // Crop overscan area from the source and center in the window
    SDL_Rect src = { v->crop_l, v->crop_t,
                     256 - v->crop_l - v->crop_r,
                     240 - v->crop_t - v->crop_b };
    int scale_x = v->w / 256; if (scale_x <= 0) scale_x = 1;
    int scale_y = v->h / 240; if (scale_y <= 0) scale_y = 1;
    SDL_Rect dst;
    dst.w = src.w * scale_x;
    dst.h = src.h * scale_y;
    dst.x = (v->w - dst.w) / 2;
    dst.y = (v->h - dst.h) / 2;
    SDL_RenderCopy(v->ren, v->tex, &src, &dst);
    SDL_RenderPresent(v->ren);
}

static SDL_Keycode key_from_name_trim(const char *name) {
    // SDL_GetKeyFromName handles many human-friendly names
    SDL_Keycode k = SDL_GetKeyFromName(name);
    return k;
}

bool video_parse_and_set_keymap(Video *v, int pad, const char *csv) {
    if (!v || !csv || (pad != 1 && pad != 2)) return false;
    char buf[256];
    strncpy(buf, csv, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
    SDL_Keycode temp[8] = {0};
    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && count < 8) {
        while (*tok == ' ' || *tok == '\t') ++tok; // trim left
        size_t len = strlen(tok);
        while (len > 0 && (tok[len-1] == ' ' || tok[len-1] == '\t')) tok[--len] = '\0';
        SDL_Keycode k = key_from_name_trim(tok);
        if (k == SDLK_UNKNOWN) return false;
        temp[count++] = k;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    if (count != 8) return false;
    if (pad == 1) memcpy(v->map1, temp, sizeof(temp)); else memcpy(v->map2, temp, sizeof(temp));
    return true;
}

#else

// Headless stubs when SDL2 is not available
struct Video { int dummy; };

bool video_init(Video **out, const char *title, int width, int height, int scale) {
    (void)out; (void)title; (void)width; (void)height; (void)scale; return false;
}
void video_fill(Video *v, uint8_t r, uint8_t g, uint8_t b) { (void)v; (void)r; (void)g; (void)b; }
void video_poll(Video *v, bool *quit, uint8_t *pad1_state, uint8_t *pad2_state) { (void)v; (void)quit; if (pad1_state) *pad1_state = 0; if (pad2_state) *pad2_state = 0; }
void video_shutdown(Video **v) { (void)v; }
void video_present(Video *v, const uint32_t *pixels) { (void)v; (void)pixels; }

#endif
