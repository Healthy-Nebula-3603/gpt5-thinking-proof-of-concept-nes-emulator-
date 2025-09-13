#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Video Video;

bool video_init(Video **out, const char *title, int width, int height, int scale);
void video_fill(Video *v, uint8_t r, uint8_t g, uint8_t b);
// Polls events; updates quit flag and current pad states (A,B,Select,Start,Up,Down,Left,Right)
void video_poll(Video *v, bool *quit, uint8_t *pad1_state, uint8_t *pad2_state);
void video_shutdown(Video **v);
// Present a 256x240 ARGB8888 buffer
void video_present(Video *v, const uint32_t *pixels);
// Set key mapping from CSV of 8 key names: A,B,Select,Start,Up,Down,Left,Right
bool video_parse_and_set_keymap(Video *v, int pad /*1 or 2*/, const char *csv);
