/*
 * 3DS Maps: an OpenStreetMap viewer for the Nintendo 3DS that uses a phone
 * as its GPS receiver (NMEA sentences over Wi-Fi).
 *
 * Top screen: the map. Bottom screen: GPS / network status and controls.
 */
#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "config.h"
#include "gps_net.h"
#include "tiles.h"
#include "render.h"
#include "nav.h"
#include "nav_net.h"



#define SOC_BUFSIZE 0x100000
#define MIN_ZOOM 2
#define MAX_ZOOM 19

static u8 *fb;  /* top screen, 240x400, column-major bottom-up, BGR */

static inline void putpx(int x, int y, u8 r, u8 g, u8 b)
{
	if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
	u8 *p = fb + (x * SCR_H + (SCR_H - 1 - y)) * 3;
	p[0] = b; p[1] = g; p[2] = r;
}

static void fill_rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b)
{
	for (int x = x0; x < x0 + w; x++)
		for (int y = y0; y < y0 + h; y++) putpx(x, y, r, g, b);
}

static void fill_circle(int cx, int cy, int rad, u8 r, u8 g, u8 b)
{
	for (int dy = -rad; dy <= rad; dy++)
		for (int dx = -rad; dx <= rad; dx++)
			if (dx * dx + dy * dy <= rad * rad) putpx(cx + dx, cy + dy, r, g, b);
}

static void draw_line(int x0, int y0, int x1, int y1, int thick, u8 r, u8 g, u8 b)
{
	int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
	if (steps == 0) steps = 1;
	for (int i = 0; i <= steps; i++) {
		int x = x0 + (x1 - x0) * i / steps, y = y0 + (y1 - y0) * i / steps;
		fill_circle(x, y, thick, r, g, b);
	}
}

/* 8x8 text using the console's built-in font. */
static void draw_text(int x, int y, const char *s, u8 r, u8 g, u8 b)
{
	const u8 *font = consoleGetDefault()->font.gfx;
	for (; *s; s++, x += 6) {
		const u8 *ch = font + (u8)*s * 8;
		for (int row = 0; row < 8; row++)
			for (int col = 0; col < 8; col++)
				if (ch[row] & (0x80 >> col)) putpx(x + col, y + row, r, g, b);
	}
}

static void draw_label(int x, int y, const char *s)
{
	int w = strlen(s) * 6 + 4;
	fill_rect(x, y, w, 11, 255, 255, 255);
	draw_text(x + 2, y + 2, s, 40, 40, 40);
}

static void draw_position(int x, int y, double course, bool moving, bool stale)
{
	u8 r = stale ? 150 : 30, g = stale ? 150 : 110, b = stale ? 150 : 230;
	if (moving && course >= 0) {
		double a = course * M_PI / 180.0;
		draw_line(x, y, x + (int)(sin(a) * 22), y - (int)(cos(a) * 22), 1, r, g, b);
	}
	fill_circle(x, y, 8, 255, 255, 255);
	fill_circle(x, y, 6, r, g, b);
}

static void draw_crosshair(void)
{
	int cx = SCR_W / 2, cy = SCR_H / 2;
	for (int i = -6; i <= 6; i++) {
		if (i > -2 && i < 2) continue;
		putpx(cx + i, cy, 220, 0, 0); putpx(cx, cy + i, 220, 0, 0);
	}
}


/* ------------------------------------------------------------------ */
/* navigation drawing                                                  */
/* ------------------------------------------------------------------ */

/* Arrow icon for a manoeuvre, centred in a 22x22 box at (x, y). */
static void draw_arrow(int x, int y, char kind, u8 r, u8 g, u8 b)
{
	int cx = x + 11, by = y + 20;
	switch (kind) {
	case 'L': case 'l':
		draw_line(cx + 4, by, cx + 4, y + 9, 1, r, g, b);
		draw_line(cx + 4, y + 9, x + 4, y + 9, 1, r, g, b);
		draw_line(x + 4, y + 9, x + 9, y + 4, 1, r, g, b);
		draw_line(x + 4, y + 9, x + 9, y + 14, 1, r, g, b);
		break;
	case 'R': case 'r':
		draw_line(cx - 4, by, cx - 4, y + 9, 1, r, g, b);
		draw_line(cx - 4, y + 9, x + 18, y + 9, 1, r, g, b);
		draw_line(x + 18, y + 9, x + 13, y + 4, 1, r, g, b);
		draw_line(x + 18, y + 9, x + 13, y + 14, 1, r, g, b);
		break;
	case 'U':
		draw_line(cx + 5, by, cx + 5, y + 7, 1, r, g, b);
		draw_line(cx + 5, y + 7, cx - 5, y + 7, 1, r, g, b);
		draw_line(cx - 5, y + 7, cx - 5, by - 4, 1, r, g, b);
		draw_line(cx - 5, by - 4, cx - 9, by - 8, 1, r, g, b);
		draw_line(cx - 5, by - 4, cx - 1, by - 8, 1, r, g, b);
		break;
	case 'O':
		for (int a = 0; a < 360; a += 10) {
			double t = a * M_PI / 180;
			fill_circle(cx + (int)(6 * cos(t)), y + 11 + (int)(6 * sin(t)), 1, r, g, b);
		}
		draw_line(cx, by, cx, y + 17, 1, r, g, b);
		break;
	case 'A':
		fill_circle(cx, y + 8, 6, r, g, b);
		draw_line(cx, y + 8, cx, by, 1, r, g, b);
		fill_circle(cx, y + 8, 2, 255, 255, 255);
		break;
	default:
		draw_line(cx, by, cx, y + 3, 1, r, g, b);
		draw_line(cx, y + 3, cx - 5, y + 8, 1, r, g, b);
		draw_line(cx, y + 3, cx + 5, y + 8, 1, r, g, b);
	}
}

static void draw_pin(int x, int y)
{
	draw_line(x, y, x, y - 12, 1, 120, 0, 0);
	fill_circle(x, y - 16, 7, 255, 255, 255);
	fill_circle(x, y - 16, 5, 220, 30, 30);
}

/* route polyline in world pixels, recomputed when the zoom or route changes */
static double *route_px, *route_py;
static int route_px_zoom = -1, route_px_n;

static void route_project(const Route *r, int z)
{
	if (route_px_zoom == z && route_px_n == r->npts) return;
	free(route_px); free(route_py);
	route_px = malloc(r->npts * sizeof(double));
	route_py = malloc(r->npts * sizeof(double));
	for (int i = 0; i < r->npts; i++) {
		route_px[i] = lon_to_px(r->lon[i], z);
		route_py[i] = lat_to_px(r->lat[i], z);
	}
	route_px_zoom = z;
	route_px_n = r->npts;
}

static void draw_route(const Route *r, int z, long wx, long wy, int from_seg)
{
	route_project(r, z);
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i + 1 < r->npts; i++) {
			double x0 = route_px[i] - wx, y0 = route_py[i] - wy;
			double x1 = route_px[i + 1] - wx, y1 = route_py[i + 1] - wy;
			if ((x0 < -10 && x1 < -10) || (x0 > SCR_W + 10 && x1 > SCR_W + 10) ||
			    (y0 < -10 && y1 < -10) || (y0 > SCR_H + 10 && y1 > SCR_H + 10)) continue;
			if (fabs(x1 - x0) > 4000 || fabs(y1 - y0) > 4000) continue;
			bool done = i < from_seg;   /* already travelled: grey */
			if (pass == 0) draw_line((int)x0, (int)y0, (int)x1, (int)y1, 3, 255, 255, 255);
			else if (done) draw_line((int)x0, (int)y0, (int)x1, (int)y1, 2, 150, 150, 170);
			else draw_line((int)x0, (int)y0, (int)x1, (int)y1, 2, 60, 90, 235);
		}
	}
}

/* ------------------------------------------------------------------ */
/* bottom screen                                                       */
/* ------------------------------------------------------------------ */

static const char *tcp_state_str(TcpState s)
{
	switch (s) {
	case TCP_OFF: return "off";
	case TCP_CONNECTING: return "trying";
	case TCP_CONNECTED: return "connected";
	default: return "waiting";
	}
}

/* print text word-wrapped to `width` columns over exactly `lines` lines */
static void print_wrapped(const char *text, int width, int lines, const char *indent)
{
	const char *s = text;
	for (int l = 0; l < lines; l++) {
		int avail = width - (int)strlen(indent);
		int n = strlen(s);
		if (n > avail) {
			n = avail;
			while (n > 0 && s[n] != ' ') n--;
			if (n == 0) n = avail;
		}
		printf("%s%-*.*s\n", indent, avail, n, s);
		s += n;
		while (*s == ' ') s++;
	}
}

static void print_gps_line(const GpsNet *g)
{
	u64 now = osGetTime();
	if (!g->last_rx_ms) printf(" GPS: \x1b[33mwaiting for phone...\x1b[0m           \n");
	else if (g->fix.valid && now - g->last_fix_ms < 5000)
		printf(" GPS: \x1b[32mFIX\x1b[0m   %5.1f km/h                 \n", g->fix.speed_kmh);
	else if (g->last_fix_ms)
		printf(" GPS: \x1b[33mLOST\x1b[0m last fix %us ago        \n", (unsigned)((now - g->last_fix_ms) / 1000));
	else printf(" GPS: \x1b[33mno fix yet\x1b[0m (phone sending)    \n");
}

static void print_status(const GpsNet *g, const Config *cfg, int z, double clat, double clon, bool follow,
                         const char *msg)
{
	uint64_t now = osGetTime();
	printf("\x1b[1;1H");
	printf("\x1b[36m 3DS Maps\x1b[0m        (c) OpenStreetMap  \n");
	printf("                     contributors     \n");
	printf("---------------------------------------\n");

	const GpsFix *f = &g->fix;
	if (!g->last_rx_ms) {
		printf(" GPS: \x1b[33mwaiting for phone...\x1b[0m           \n");
		printf("                                       \n");
		printf("                                       \n");
	} else {
		unsigned age = (unsigned)((now - g->last_rx_ms) / 1000);
		if (f->valid && now - g->last_fix_ms < 5000)
			printf(" GPS: \x1b[32mFIX\x1b[0m  data %us ago             \n", age);
		else if (g->last_fix_ms)
			printf(" GPS: \x1b[33mLOST\x1b[0m last fix %us ago        \n",
			       (unsigned)((now - g->last_fix_ms) / 1000));
		else
			printf(" GPS: \x1b[33mno fix yet\x1b[0m (phone sending)    \n");
		printf(" Lat %10.5f  Lon %11.5f      \n", f->lat, f->lon);
		printf(" Speed %5.1f km/h  Course %5.0f    \n", f->speed_kmh, f->course_deg < 0 ? 0 : f->course_deg);
	}
	printf(" From: %-30s\n", g->source);
	printf("---------------------------------------\n");
	if (g->web_port) {
		char url[40];
		snprintf(url, sizeof(url), "http://%s:%d", g->my_ip, g->web_port);
		printf(" Phone page: \x1b[32m%-26s\x1b[0m\n", url);
	} else printf(" Web page: off                          \n");
	printf(" 3DS IP:  %-15s UDP %-5d     \n", g->my_ip, cfg->udp_port);
	if (g->tcp_state != TCP_OFF)
		printf(" Phone: %-21s %-9.9s\n", g->tcp_target, tcp_state_str(g->tcp_state));
	else
		printf(" Phone TCP: off                         \n");
	printf("---------------------------------------\n");
	printf(" Map  z%-2d  %9.5f, %10.5f      \n", z, clat, clon);
	printf(" Tiles: %s  loading %-2d  dl %-5d    \n",
	       tiles_online() ? "\x1b[32monline \x1b[0m" : "\x1b[33moffline\x1b[0m",
	       tiles_pending(), tiles_downloaded());
	printf(" %-38.38s\n", tiles_last_error());
	printf(" Mode: %-10s                      \n", follow ? "follow GPS" : "free look");
	printf("---------------------------------------\n");
	if (msg && *msg) print_wrapped(msg, 39, 2, " ");
	else {
		printf(" \x1b[32mX: search a place / get directions\x1b[0m   \n");
		printf("                                       \n");
	}
	printf(" Pad/touch: move   L/R: zoom -/+       \n");
	printf(" A: follow GPS  SELECT: tiles on/off   \n");
	printf(" START: quit                           \n");
}

static void print_results(const Place *pl, int n, int sel, const char *query, const char *msg)
{
	printf("\x1b[1;1H");
	printf("\x1b[36m Search:\x1b[0m %-30.30s\n", query);
	printf("---------------------------------------\n");
	if (n == 0) {
		printf(" No places found.                      \n");
		for (int i = 0; i < 21; i++) printf("                                       \n");
	} else {
		for (int i = 0; i < MAX_PLACES; i++) {
			if (i < n) {
				printf("%s%c %-36.36s\x1b[0m\n", i == sel ? "\x1b[47;30m" : "", i == sel ? '>' : ' ', pl[i].name);
				printf("   \x1b[37m%-35.35s\x1b[0m\n", pl[i].detail);
			} else {
				printf("                                       \n");
				printf("                                       \n");
			}
		}
	}
	printf("---------------------------------------\n");
	if (msg && *msg) print_wrapped(msg, 39, 2, " ");
	else { printf("                                       \n"); printf("                                       \n"); }
	printf(" Up/Down: choose   B: back             \n");
	printf(" \x1b[32mA: directions by car\x1b[0m                  \n");
	printf(" \x1b[32mY: directions walking\x1b[0m                 \n");
	printf(" Search: Nominatim / OpenStreetMap     \n");
}

static void print_nav(const GpsNet *g, const Route *r, const NavProgress *p, const Place *dest,
                      bool walking, bool have_pos, const char *msg)
{
	char d1[24], d2[24];
	printf("\x1b[1;1H");
	printf("\x1b[36m Directions\x1b[0m (%s) to:               \n", walking ? "walking" : "car");
	print_wrapped(dest->name, 39, 1, " ");
	printf("---------------------------------------\n");
	if (!have_pos) {
		printf(" \x1b[33mNo GPS position yet.\x1b[0m                  \n");
		printf(" Route starts from the map centre.     \n");
		printf("                                       \n");
		printf("                                       \n");
	} else if (p->arrived) {
		printf(" \x1b[32mYou have arrived!\x1b[0m                     \n");
		printf("                                       \n");
		printf("                                       \n");
		printf("                                       \n");
	} else {
		format_distance(d1, sizeof(d1), p->to_next_m);
		printf(" In \x1b[32m%-10s\x1b[0m                         \n", d1);
		print_wrapped(r->steps[p->next_step].text, 39, 3, " ");
	}
	printf("---------------------------------------\n");
	if (have_pos && !p->arrived && p->next_step + 1 < r->nsteps) {
		printf(" Then:                                 \n");
		print_wrapped(r->steps[p->next_step + 1].text, 39, 2, " ");
	} else {
		printf("                                       \n");
		printf("                                       \n");
		printf("                                       \n");
	}
	printf("---------------------------------------\n");
	double rem_m = have_pos ? p->remaining_m : r->distance;
	double rem_s = have_pos ? p->remaining_s : r->duration;
	format_distance(d2, sizeof(d2), rem_m);
	int mins = (int)(rem_s / 60.0 + 0.5);
	if (mins >= 60) printf(" Left: %-9s  about %dh %02dmin         \n", d2, mins / 60, mins % 60);
	else printf(" Left: %-9s  about %d min              \n", d2, mins);
	if (have_pos && p->off_route_m > 30 && !p->arrived)
		printf(" \x1b[33mOff route by %d m\x1b[0m                     \n", (int)p->off_route_m);
	else printf("                                       \n");
	print_gps_line(g);
	printf("---------------------------------------\n");
	if (msg && *msg) print_wrapped(msg, 39, 2, " ");
	else { printf("                                       \n"); printf("                                       \n"); }
	printf(" B: end route    X: new search         \n");
	printf(" A: follow me  L/R: zoom  START: quit  \n");
	printf(" Route: OSRM / FOSSGIS / OpenStreetMap \n");
	printf("                                       \n");
}

/* ------------------------------------------------------------------ */

typedef enum { UI_MAP, UI_RESULTS } UiMode;

static bool ask_search_text(char *out, int n, const char *last)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, n - 1);
	swkbdSetHintText(&kb, "Where do you want to go?");
	swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Search", true);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (last && *last) swkbdSetInitialText(&kb, last);
	char buf[128] = {0};
	SwkbdButton b = swkbdInputText(&kb, buf, sizeof(buf));
	if (b != SWKBD_BUTTON_RIGHT || !buf[0]) return false;
	snprintf(out, n, "%s", buf);
	return true;
}

int main(void)
{
	osSetSpeedupEnable(true);   /* New 3DS: 804 MHz + L2 cache */
	gfxInitDefault();
	gfxSet3D(false);
	consoleInit(GFX_BOTTOM, NULL);
	romfsInit();

	u32 *soc_buf = memalign(0x1000, SOC_BUFSIZE);
	bool net_ok = soc_buf && socInit(soc_buf, SOC_BUFSIZE) == 0;

	Config cfg;
	config_load(&cfg, APP_DIR "/config.ini");

	double lat = cfg.start_lat, lon = cfg.start_lon;
	int z = cfg.zoom;
	state_load(&lat, &lon, &z);

	GpsNet gps;
	if (net_ok) gps_net_init(&gps, &cfg);
	else { memset(&gps, 0, sizeof(gps)); nmea_init(&gps.fix); strcpy(gps.my_ip, "no network"); }
	tiles_init(&cfg);
	if (!net_ok) tiles_set_online(false);
	navnet_init(cfg.user_agent);

	/* view centre in world pixels at zoom z */
	double cx = lon_to_px(lon, z), cy = lat_to_px(lat, z);
	bool follow = true;
	int frame = 0;
	touchPosition last_touch = {0};
	bool touching = false;

	/* search / navigation state */
	UiMode ui = UI_MAP, last_ui = UI_MAP;
	bool last_nav = false;
	char query[128] = "", msg[96] = "";
	Place places[MAX_PLACES];
	int nplaces = 0, sel = 0;
	Route route = {0};
	bool have_route = false, walking = false, rerouting = false;
	Place dest = {0};
	NavProgress prog = {0};
	u64 off_since = 0, last_route_req = 0;

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;

		if (net_ok) gps_net_poll(&gps);
		bool have_fix = gps.fix.valid && gps.last_fix_ms && osGetTime() - gps.last_fix_ms < 30000;
		u64 now = osGetTime();

		/* ---- finished network requests ---- */
		JobState js = navnet_state();
		if (js == JOB_FAILED) {
			snprintf(msg, sizeof(msg), "%s", navnet_error());
			rerouting = false;
			navnet_clear();
		} else if (js == JOB_DONE) {
			if (navnet_kind() == JOB_SEARCH) {
				nplaces = nav_parse_search(navnet_body(), places, MAX_PLACES);
				if (nplaces < 0) { nplaces = 0; snprintf(msg, sizeof(msg), "Search failed: bad reply"); }
				else msg[0] = 0;
				sel = 0;
				ui = UI_RESULTS;
			} else {
				Route nr;
				char err[64];
				if (nav_parse_route(navnet_body(), &nr, err, sizeof(err))) {
					if (have_route) route_free(&route);
					route = nr;
					have_route = true;
					route_px_zoom = -1;
					memset(&prog, 0, sizeof(prog));
					off_since = 0;
					msg[0] = 0;
					if (!rerouting) { follow = true; ui = UI_MAP; }
				} else {
					snprintf(msg, sizeof(msg), "Route failed: %s", err);
				}
				rerouting = false;
			}
			navnet_clear();
		}

		/* ---- input ---- */
		if (ui == UI_RESULTS) {
			if ((down & KEY_DOWN) && sel + 1 < nplaces) sel++;
			if ((down & KEY_UP) && sel > 0) sel--;
			if (down & KEY_B) { ui = UI_MAP; msg[0] = 0; }
			if ((down & (KEY_A | KEY_Y)) && nplaces > 0 && navnet_state() == JOB_IDLE) {
				dest = places[sel];
				walking = (down & KEY_Y) != 0;
				double flat = have_fix ? gps.fix.lat : px_to_lat(cy, z);
				double flon = have_fix ? gps.fix.lon : px_to_lon(cx, z);
				navnet_route(flat, flon, dest.lat, dest.lon, walking);
				last_route_req = now;
				snprintf(msg, sizeof(msg), "Getting directions...");
			}
		} else {
			/* zoom keeps the screen centre fixed */
			int nz = z;
			if (down & (KEY_R | KEY_ZR)) nz = z < MAX_ZOOM ? z + 1 : z;
			if (down & (KEY_L | KEY_ZL)) nz = z > MIN_ZOOM ? z - 1 : z;
			if (nz != z) {
				double f = nz > z ? 2.0 : 0.5;
				cx *= f; cy *= f; z = nz;
			}

			/* panning: circle pad (analog), d-pad, touch drag */
			circlePosition cp;
			hidCircleRead(&cp);
			double dx = 0, dy = 0;
			if (abs(cp.dx) > 15) dx += cp.dx / 16.0;
			if (abs(cp.dy) > 15) dy -= cp.dy / 16.0;
			if (held & KEY_DLEFT) dx -= 4;
			if (held & KEY_DRIGHT) dx += 4;
			if (held & KEY_DUP) dy -= 4;
			if (held & KEY_DDOWN) dy += 4;
			if (held & KEY_TOUCH) {
				touchPosition t;
				hidTouchRead(&t);
				if (touching) { dx -= t.px - last_touch.px; dy -= t.py - last_touch.py; }
				last_touch = t;
				touching = true;
			} else touching = false;
			if (dx != 0 || dy != 0) { cx += dx; cy += dy; follow = false; }

			if (down & KEY_A) follow = true;
			if (down & KEY_SELECT) tiles_set_online(!tiles_online());
			if ((down & KEY_B) && have_route) {
				route_free(&route);
				have_route = false;
				msg[0] = 0;
			}
			if (down & KEY_X) {
				if (!net_ok) snprintf(msg, sizeof(msg), "Search needs Wi-Fi (connect to the phone hotspot).");
				else if (navnet_state() == JOB_RUNNING) snprintf(msg, sizeof(msg), "Please wait...");
				else if (ask_search_text(query, sizeof(query), query)) {
					double slat = have_fix ? gps.fix.lat : px_to_lat(cy, z);
					double slon = have_fix ? gps.fix.lon : px_to_lon(cx, z);
					navnet_search(query, slat, slon);
					snprintf(msg, sizeof(msg), "Searching for \"%.60s\"...", query);
				}
			}
		}

		/* ---- navigation progress & rerouting ---- */
		if (have_route && have_fix) {
			nav_progress(&route, gps.fix.lat, gps.fix.lon, &prog);
			double limit = walking ? 35.0 : 60.0;
			if (prog.off_route_m > limit && !prog.arrived) {
				if (!off_since) off_since = now;
				if (now - off_since > 6000 && now - last_route_req > 15000 && navnet_state() == JOB_IDLE) {
					navnet_route(gps.fix.lat, gps.fix.lon, dest.lat, dest.lon, walking);
					last_route_req = now;
					rerouting = true;
					snprintf(msg, sizeof(msg), "Off route - finding a new route...");
				}
			} else off_since = 0;
		}

		if (follow && have_fix) {
			cx = lon_to_px(gps.fix.lon, z);
			cy = lat_to_px(gps.fix.lat, z);
		}
		double world = (double)((long)TILE_SIZE << z);
		cx = fmod(fmod(cx, world) + world, world);
		if (cy < 0) cy = 0;
		if (cy > world) cy = world;

		/* ---- draw top screen ---- */
		tiles_begin_frame();
		fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
		long wx = (long)floor(cx) - SCR_W / 2, wy = (long)floor(cy) - SCR_H / 2;
		draw_map(fb, z, wx, wy);

		/* load a ring of tiles around the screen, and the zoomed-out view, so
		 * panning and zooming show map (or a blurry preview) straight away */
		{
			int tx0 = (int)floor(wx / 256.0), tx1 = (int)floor((wx + SCR_W - 1) / 256.0);
			int ty0 = (int)floor(wy / 256.0), ty1 = (int)floor((wy + SCR_H - 1) / 256.0);
			if (z > MIN_ZOOM)
				for (int ty = ty0 >> 1; ty <= ty1 >> 1; ty++)
					for (int tx = tx0 >> 1; tx <= tx1 >> 1; tx++) tiles_prefetch(z - 1, tx, ty);
			for (int ty = ty0 - 1; ty <= ty1 + 1; ty++)
				for (int tx = tx0 - 1; tx <= tx1 + 1; tx++)
					if (ty < ty0 || ty > ty1 || tx < tx0 || tx > tx1) tiles_prefetch(z, tx, ty);
		}

		if (have_route) {
			draw_route(&route, z, wx, wy, have_fix ? prog.seg : 0);
			draw_pin((int)(lon_to_px(dest.lon, z) - wx), (int)(lat_to_px(dest.lat, z) - wy));
		}
		if (ui == UI_RESULTS) {
			for (int i = 0; i < nplaces; i++) {
				int px = (int)(lon_to_px(places[i].lon, z) - wx), py = (int)(lat_to_px(places[i].lat, z) - wy);
				if (i == sel) draw_pin(px, py);
				else { fill_circle(px, py, 4, 255, 255, 255); fill_circle(px, py, 3, 200, 60, 60); }
			}
		}

		if (have_fix) {
			double gx = lon_to_px(gps.fix.lon, z) - wx, gy = lat_to_px(gps.fix.lat, z) - wy;
			/* nearest copy across the date line */
			if (gx < -world / 2) gx += world;
			if (gx > world / 2) gx -= world;
			bool stale = now - gps.last_fix_ms > 5000;
			draw_position((int)gx, (int)gy, gps.fix.course_deg, gps.fix.speed_kmh > 2.0, stale);
		}
		if (!follow || !have_fix) draw_crosshair();

		if (have_route && have_fix) {
			/* instruction banner */
			char line[80], d[24];
			const RouteStep *st = &route.steps[prog.next_step];
			fill_rect(0, 0, SCR_W, 26, 30, 90, 60);
			if (prog.arrived) {
				draw_arrow(4, 2, 'A', 255, 255, 255);
				draw_text(32, 9, "You have arrived!", 255, 255, 255);
			} else {
				draw_arrow(4, 2, st->arrow, 255, 255, 255);
				format_distance(d, sizeof(d), prog.to_next_m);
				snprintf(line, sizeof(line), "%s: %.52s", d, st->text);
				draw_text(32, 9, line, 255, 255, 255);
			}
		} else {
			char lbl[48];
			snprintf(lbl, sizeof(lbl), "z%d%s%s", z, follow && have_fix ? " follow" : "",
		         tiles_online() ? "" : "  OFFLINE - press SELECT to load map");
			draw_label(2, 2, lbl);
		}
		draw_label(SCR_W - 6 * 26 - 6, SCR_H - 13, "(c) OpenStreetMap contrib.");

		/* ---- bottom screen ---- */
		bool nav_now = have_route && ui == UI_MAP;
		if (ui != last_ui || nav_now != last_nav) {
			printf("\x1b[2J");   /* layout changed: clear */
			last_ui = ui;
			last_nav = nav_now;
			frame = 0;
		}
		if (frame++ % 10 == 0) {
			if (ui == UI_RESULTS) print_results(places, nplaces, sel, query, msg);
			else if (have_route) print_nav(&gps, &route, &prog, &dest, walking, have_fix, msg);
			else print_status(&gps, &cfg, z, px_to_lat(cy, z), px_to_lon(cx, z), follow, msg);
		}

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	state_save(px_to_lat(cy, z), px_to_lon(cx, z), z);
	navnet_exit();
	tiles_exit();
	if (have_route) route_free(&route);
	if (net_ok) { gps_net_exit(&gps); socExit(); }
	romfsExit();
	gfxExit();
	return 0;
}
