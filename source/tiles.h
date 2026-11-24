#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define TILE_SIZE 256

/* Web Mercator helpers (slippy-map pixel coordinates at a zoom level). */
double lon_to_px(double lon, int z);
double lat_to_px(double lat, int z);
double px_to_lon(double x, int z);
double px_to_lat(double y, int z);

void tiles_init(const Config *cfg);
void tiles_exit(void);

/* Returns pixel data for a tile if it is decoded and in memory, otherwise
 * requests it (from SD, or the network when online) and returns NULL.
 * The data is stored column-major and bottom-up (see tiles.c) so it can be
 * memcpy'd straight into the 3DS framebuffer. Valid until the next tiles_begin_frame. */
const uint8_t *tiles_get(int z, int x, int y, bool *missing);

/* Ask for a tile just outside the screen (lower priority than visible tiles). */
void tiles_prefetch(int z, int x, int y);

/* A tile only if it is already in memory; never queues a load. */
const uint8_t *tiles_peek(int z, int x, int y);

/* Mark the start of a frame (for LRU bookkeeping). Returns true if any tile
 * finished loading since the previous call. */
bool tiles_begin_frame(void);

void tiles_set_online(bool on);
bool tiles_online(void);

/* Stats for the status screen. */
int  tiles_pending(void);
int  tiles_downloaded(void);
const char *tiles_last_error(void);
