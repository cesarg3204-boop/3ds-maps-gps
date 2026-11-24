/* Map blitting, kept free of libctru calls so it can be unit-tested on a PC. */
#include "render.h"
#include <string.h>
#include <math.h>

/* ---------- Web Mercator ---------- */

double lon_to_px(double lon, int z)
{
	return (lon + 180.0) / 360.0 * (TILE_SIZE * (double)(1 << z));
}

double lat_to_px(double lat, int z)
{
	if (lat > 85.0511) lat = 85.0511;
	if (lat < -85.0511) lat = -85.0511;
	double r = lat * M_PI / 180.0;
	return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (TILE_SIZE * (double)(1 << z));
}

double px_to_lon(double x, int z)
{
	return x / (TILE_SIZE * (double)(1 << z)) * 360.0 - 180.0;
}

double px_to_lat(double y, int z)
{
	double n = M_PI - 2.0 * M_PI * y / (TILE_SIZE * (double)(1 << z));
	return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}


/* Draw the map with its top-left corner at world pixel (wx, wy). */
void draw_map(uint8_t *fb, int z, long wx, long wy)
{
	long world = (long)TILE_SIZE << z;
	int ntiles = 1 << z;
	for (int sx = 0; sx < SCR_W; ) {
		long px = ((wx + sx) % world + world) % world;   /* wrap around the date line */
		int tx = px / TILE_SIZE, u = px % TILE_SIZE;
		int span_x = TILE_SIZE - u;
		if (span_x > SCR_W - sx) span_x = SCR_W - sx;

		for (int sy = 0; sy < SCR_H; ) {
			long py = wy + sy;
			int span_y;
			const uint8_t *tile = NULL;
			bool missing = false;
			const uint8_t *parent = NULL;
			int v = 0, ty = 0, k = 0;
			if (py < 0) span_y = (int)(-py);
			else if (py >= world) span_y = SCR_H - sy;
			else {
				ty = py / TILE_SIZE;
				v = py % TILE_SIZE;
				span_y = TILE_SIZE - v;
				if (ty < ntiles) tile = tiles_get(z, tx, ty, &missing);
				/* not loaded yet: stretch a lower-zoom tile we already have */
				for (k = 1; !tile && !parent && k <= 6 && z - k >= 0; k++)
					parent = tiles_peek(z - k, tx >> k, ty >> k);
				k--;
			}
			if (span_y > SCR_H - sy) span_y = SCR_H - sy;

			for (int i = 0; i < span_x; i++) {
				uint8_t *dst = fb + ((sx + i) * SCR_H + (SCR_H - sy - span_y)) * 3;
				if (tile) {
					/* tile column (u+i), rows v .. v+span_y-1, stored bottom-up */
					const uint8_t *src = tile + ((u + i) * TILE_SIZE + (TILE_SIZE - v - span_y)) * 3;
					memcpy(dst, src, span_y * 3);
				} else if (parent) {
					int pu = (((long)tx * TILE_SIZE + u + i) >> k) & (TILE_SIZE - 1);
					const uint8_t *col = parent + pu * TILE_SIZE * 3;
					long row0 = (long)ty * TILE_SIZE + v + span_y - 1;   /* world row of dst[0] */
					for (int j = 0; j < span_y; j++) {
						int pv = ((row0 - j) >> k) & (TILE_SIZE - 1);
						const uint8_t *sp = col + (TILE_SIZE - 1 - pv) * 3;
						dst[j * 3] = sp[0]; dst[j * 3 + 1] = sp[1]; dst[j * 3 + 2] = sp[2];
					}
				} else {
					/* placeholder: light grey (darker if the tile is unavailable) with a grid */
					uint8_t c = missing ? 0xC8 : 0xE0;
					memset(dst, c, span_y * 3);
					int col = (u + i) % 32 == 0;
					for (int j = 0; j < span_y; j++)
						if (col || (v + span_y - 1 - j) % 32 == 0) memset(dst + j * 3, 0xD0 - (missing ? 0x20 : 0), 3);
				}
			}
			sy += span_y;
		}
		sx += span_x;
	}
}


void tile_transpose(const uint8_t *rows, uint8_t *out)
{
	/* row-major top-down (as decoded) -> column-major bottom-up (framebuffer order) */
	for (int v = 0; v < TILE_SIZE; v++) {
		const uint8_t *row = rows + v * TILE_SIZE * 3;
		for (int u = 0; u < TILE_SIZE; u++) {
			uint8_t *d = out + (u * TILE_SIZE + (TILE_SIZE - 1 - v)) * 3;
			d[0] = row[u * 3 + 0];
			d[1] = row[u * 3 + 1];
			d[2] = row[u * 3 + 2];
		}
	}
}
