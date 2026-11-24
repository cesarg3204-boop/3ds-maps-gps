#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tiles.h"

#define SCR_W 400
#define SCR_H 240

/* Draw the map at zoom z with world pixel (wx, wy) at the top-left of the
 * 400x240 top screen. fb is the top framebuffer (column-major, bottom-up, BGR). */
void draw_map(uint8_t *fb, int z, long wx, long wy);

/* Convert a decoded 256x256 BGR tile (row-major, top-down) to framebuffer order. */
void tile_transpose(const uint8_t *rows, uint8_t *out);
