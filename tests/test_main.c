/* PC unit tests: NMEA parsing, map blitting into the rotated 3DS framebuffer,
 * and Web Mercator maths.  Build & run:  make -C tests  */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/nmea.h"
#include "../source/render.h"
#include "../source/web_gps.h"
#include "../source/nav.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, eps) CHECK(fabs((a) - (b)) < (eps))

/* ---- fake tile source: colour encodes the world pixel position ---- */
static uint8_t rows[TILE_SIZE * TILE_SIZE * 3], tiles[8][TILE_SIZE * TILE_SIZE * 3];
static int ntile_calls;

static int missing_zoom = -1;   /* tiles_get pretends this zoom isn't loaded yet */

static const uint8_t *make_tile(int x, int y);

const uint8_t *tiles_get(int z, int x, int y, bool *missing)
{
	*missing = false;
	(void)z;
	if (z == missing_zoom) return NULL;
	return make_tile(x, y);
}

const uint8_t *tiles_peek(int z, int x, int y)
{
	return z == missing_zoom - 1 ? make_tile(x, y) : NULL;
}

static const uint8_t *make_tile(int x, int y)
{
	uint8_t *t = tiles[ntile_calls++ % 8];
	for (int v = 0; v < TILE_SIZE; v++)
		for (int u = 0; u < TILE_SIZE; u++) {
			uint8_t *p = rows + (v * TILE_SIZE + u) * 3;
			p[0] = (uint8_t)u;             /* B = column in tile */
			p[1] = (uint8_t)v;             /* G = row in tile */
			p[2] = (uint8_t)(x * 16 + y);  /* R = tile id */
		}
	tile_transpose(rows, t);
	return t;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { printf("missing %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *b = malloc(n + 1);
	if (fread(b, 1, n, f) != (size_t)n) exit(1);
	b[n] = 0; fclose(f);
	return b;
}

static void test_nav(void)
{
	char buf[128];
	utf8_to_ascii(buf, sizeof(buf), "Calle Almad\xc3\xa9n, Espa\xc3\xb1""a \xc2\xbfS\xc3\xad?");
	CHECK(!strcmp(buf, "Calle Almaden, Espana Si?"));
	url_encode(buf, sizeof(buf), "Museo del Prado & co");
	CHECK(!strcmp(buf, "Museo+del+Prado+%26+co"));
	format_distance(buf, sizeof(buf), 12.4);   CHECK(!strcmp(buf, "12 m"));
	format_distance(buf, sizeof(buf), 347);    CHECK(!strcmp(buf, "350 m"));
	format_distance(buf, sizeof(buf), 1254.1); CHECK(!strcmp(buf, "1.3 km"));

	/* search */
	Place pl[MAX_PLACES];
	char *js = slurp("fixtures/search_prado.json");
	int n = nav_parse_search(js, pl, MAX_PLACES);
	CHECK(n == 2);
	CHECK(!strcmp(pl[0].name, "Prado Museum"));
	CHECK(strstr(pl[0].detail, "Jeronimos, Retiro, Madrid") != NULL);
	NEAR(pl[0].lat, 40.4137925, 1e-7);
	NEAR(pl[0].lon, -3.6920407, 1e-7);
	CHECK(!strcmp(pl[1].name, "12"));
	CHECK(!strncmp(pl[1].detail, "Calle de Alcala", 15));
	free(js);
	CHECK(nav_parse_search("[]", pl, MAX_PLACES) == 0);
	CHECK(nav_parse_search("<html>error</html>", pl, MAX_PLACES) < 0);

	/* route (real OSRM reply, walking, Madrid) */
	Route r;
	char err[64];
	js = slurp("fixtures/route_foot_madrid.json");
	CHECK(nav_parse_route(js, &r, err, sizeof(err)));
	free(js);
	NEAR(r.lat[0], 40.40737, 1e-5);
	NEAR(r.lon[0], -3.69797, 1e-5);
	NEAR(r.lat[r.npts - 1], 40.41389, 1e-5);
	NEAR(r.lon[r.npts - 1], -3.69177, 1e-5);
	NEAR(r.cum[r.npts - 1], 1254.1, 40.0);   /* our geometry length vs server's distance */
	CHECK(r.nsteps == 8);
	CHECK(!strcmp(r.steps[0].text, "Head east on Calle del Doctor Fourquet"));
	CHECK(!strcmp(r.steps[1].text, "Turn right onto Calle Mallorca"));
	CHECK(!strcmp(r.steps[2].text, "Turn sharp left"));
	CHECK(!strcmp(r.steps[4].text, "Turn right onto Calle Almaden"));
	CHECK(!strcmp(r.steps[5].text, "Keep left at the fork"));
	CHECK(!strcmp(r.steps[7].text, "Arrive at your destination"));
	for (int i = 1; i < r.nsteps; i++) CHECK(r.steps[i].geom_idx >= r.steps[i - 1].geom_idx);
	CHECK(r.steps[7].geom_idx == r.npts - 1);

	NavProgress p = {0};
	nav_progress(&r, 40.407368, -3.697974, &p);       /* at the start */
	CHECK(p.next_step == 1);
	CHECK(p.off_route_m < 2);
	CHECK(p.to_next_m > 5 && p.to_next_m < 15);
	CHECK(!p.arrived);
	nav_progress(&r, 40.40740, -3.69780, &p);        /* just past the first turn, on Calle Mallorca */
	CHECK(p.next_step >= 2);
	nav_progress(&r, 40.4100, -3.6900, &p);           /* far off the route */
	CHECK(p.off_route_m > 200);
	p.seg = 0;
	nav_progress(&r, 40.413885, -3.691771, &p);       /* at the destination */
	CHECK(p.arrived);
	CHECK(p.next_step == 7);
	route_free(&r);

	CHECK(!nav_parse_route("{\"code\":\"NoRoute\",\"message\":\"Impossible route\"}", &r, err, sizeof(err)));
	CHECK(!strcmp(err, "no route found"));
	CHECK(!nav_parse_route("garbage", &r, err, sizeof(err)));
}

static void check_render(int z, long wx, long wy)
{
	static uint8_t fb[SCR_W * SCR_H * 3];
	long world = (long)TILE_SIZE << z;
	memset(fb, 0x55, sizeof(fb));
	draw_map(fb, z, wx, wy);
	int bad = 0;
	for (int x = 0; x < SCR_W; x++)
		for (int y = 0; y < SCR_H; y++) {
			long py = wy + y;
			if (py < 0 || py >= world) continue;   /* outside the world: placeholder */
			long px = ((wx + x) % world + world) % world;
			const uint8_t *p = fb + (x * SCR_H + (SCR_H - 1 - y)) * 3;
			uint8_t eb = px % 256, eg = py % 256, er = (uint8_t)((px / 256) * 16 + py / 256);
			if (p[0] != eb || p[1] != eg || p[2] != er) {
				if (bad++ < 3) printf("  z%d (%ld,%ld) screen %d,%d got %d,%d,%d want %d,%d,%d\n",
				                      z, wx, wy, x, y, p[2], p[1], p[0], er, eg, eb);
			}
		}
	CHECK(bad == 0);
}

int main(void)
{
	GpsFix f;

	/* RMC with valid checksum */
	nmea_init(&f);
	CHECK(nmea_parse_sentence(&f, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n"));
	CHECK(f.valid);
	NEAR(f.lat, 48.1173, 1e-4);
	NEAR(f.lon, 11.516667, 1e-4);
	NEAR(f.speed_kmh, 22.4 * 1.852, 1e-6);
	NEAR(f.course_deg, 84.4, 1e-6);
	CHECK(f.hh == 12 && f.mm == 35 && f.ss == 19);

	/* bad checksum rejected */
	nmea_init(&f);
	CHECK(!nmea_parse_sentence(&f, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B"));
	CHECK(!f.valid);

	/* GGA, southern/western hemisphere, GN talker */
	nmea_init(&f);
	CHECK(nmea_parse_sentence(&f, "$GNGGA,010203.00,3351.000,S,15112.600,W,1,09,0.9,58.0,M,,,,"));
	CHECK(f.valid);
	NEAR(f.lat, -33.85, 1e-6);
	NEAR(f.lon, -151.21, 1e-6);
	CHECK(f.sats == 9);
	NEAR(f.hdop, 0.9, 1e-9);
	NEAR(f.altitude_m, 58.0, 1e-9);

	/* void RMC clears validity */
	CHECK(nmea_parse_sentence(&f, "$GPRMC,010204,V,,,,,,,,,"));
	CHECK(!f.valid);

	/* unknown sentence ignored */
	CHECK(!nmea_parse_sentence(&f, "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74"));

	/* streaming: split across chunks, CRLF, junk */
	NmeaStream s;
	nmea_stream_init(&s);
	nmea_init(&f);
	const char *a = "garbage$GPRMC,123519,A,4807.0";
	const char *b = "38,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n$GNGGA,010203.00,3351.000,S,15112.600,W,1,09,0.9,58.0,M,,,,\n";
	CHECK(nmea_stream_feed(&s, &f, a, strlen(a)) == 0);
	CHECK(nmea_stream_feed(&s, &f, b, strlen(b)) == 2);
	NEAR(f.lat, -33.85, 1e-6);

	/* phone browser POST body */
	nmea_init(&f);
	CHECK(web_gps_parse(&f, "8.98340,-79.51990,12.5,1.5,270,15"));
	CHECK(f.valid);
	NEAR(f.lat, 8.9834, 1e-9);
	NEAR(f.lon, -79.5199, 1e-9);
	NEAR(f.speed_kmh, 5.4, 1e-9);
	NEAR(f.course_deg, 270, 1e-9);
	NEAR(f.altitude_m, 15, 1e-9);
	nmea_init(&f);
	CHECK(web_gps_parse(&f, "8.9834,-79.5199,30,,,"));   /* speed/heading/alt unknown */
	CHECK(f.valid && f.course_deg < 0 && f.speed_kmh == 0);
	CHECK(!web_gps_parse(&f, ",-79.5,10,,,"));
	CHECK(!web_gps_parse(&f, "95,10,10,,,"));
	CHECK(!web_gps_parse(&f, "abc"));

	/* Web Mercator round trip + known value (Madrid, z15 tile 16046/12355, checked with Python) */
	for (int z = 2; z <= 19; z += 5) {
		NEAR(px_to_lat(lat_to_px(40.4168, z), z), 40.4168, 1e-9);
		NEAR(px_to_lon(lon_to_px(-3.7038, z), z), -3.7038, 1e-9);
	}
	CHECK((int)(lon_to_px(-3.7038, 15) / 256) == 16046);
	CHECK((int)(lat_to_px(40.4168, 15) / 256) == 12355);

	test_nav();

	/* blitting: unaligned offsets, tile seams, date-line wrap, top/bottom edges */
	check_render(3, 0, 0);
	check_render(3, 37, 91);
	check_render(3, 255, 255);
	check_render(3, -123, 300);         /* wraps across the date line */
	check_render(3, 2048 - 100, 1000);
	check_render(2, 900, -50);          /* above the north edge */
	check_render(2, 500, 1024 - 100);   /* below the south edge */
	check_render(1, 0, 0);

	/* zoom 4 not loaded: zoom 3 tile is stretched 2x */
	{
		static uint8_t fb[SCR_W * SCR_H * 3];
		missing_zoom = 4;
		long wx = 1000, wy = 777;
		draw_map(fb, 4, wx, wy);
		int bad = 0;
		for (int x = 0; x < SCR_W; x++)
			for (int y = 0; y < SCR_H; y++) {
				long px = (wx + x) >> 1, py = (wy + y) >> 1;   /* zoom-3 world pixel */
				const uint8_t *p = fb + (x * SCR_H + (SCR_H - 1 - y)) * 3;
				uint8_t eb = px % 256, eg = py % 256, er = (uint8_t)((px / 256) * 16 + py / 256);
				if (p[0] != eb || p[1] != eg || p[2] != er) bad++;
			}
		CHECK(bad == 0);
		missing_zoom = -1;
	}              /* world (512px) narrower than... still wider than 400 */

	if (fails) { printf("%d check(s) failed\n", fails); return 1; }
	printf("all tests passed\n");
	return 0;
}
