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
#include <time.h>

#include "config.h"
#include "gps_net.h"
#include "tiles.h"
#include "render.h"
#include "nav.h"
#include "nav_net.h"



#define SOC_BUFSIZE 0x100000
#define MIN_ZOOM 2
#define MAX_ZOOM 19

/* Current drawing target. Both screens are 240 pixels tall and stored rotated
 * (column-major, bottom-up, BGR): top is 400 wide, bottom 320. */
static u8 *fb;
static int cur_w = SCR_W;

static void use_screen(gfxScreen_t s)
{
	fb = gfxGetFramebuffer(s, GFX_LEFT, NULL, NULL);
	cur_w = s == GFX_TOP ? SCR_W : 320;
}

static inline void putpx(int x, int y, u8 r, u8 g, u8 b)
{
	if ((unsigned)x >= (unsigned)cur_w || (unsigned)y >= SCR_H) return;
	u8 *p = fb + (x * SCR_H + (SCR_H - 1 - y)) * 3;
	p[0] = b; p[1] = g; p[2] = r;
}

static void fill_rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b)
{
	if (x0 < 0) { w += x0; x0 = 0; }
	if (y0 < 0) { h += y0; y0 = 0; }
	if (x0 + w > cur_w) w = cur_w - x0;
	if (y0 + h > SCR_H) h = SCR_H - y0;
	if (w <= 0 || h <= 0) return;
	for (int x = x0; x < x0 + w; x++) {
		u8 *p = fb + (x * SCR_H + (SCR_H - y0 - h)) * 3;
		for (int j = 0; j < h; j++, p += 3) { p[0] = b; p[1] = g; p[2] = r; }
	}
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

/* Text using libctru's built-in 8x8 console font, at 1x, 2x or 3x. */
static void draw_text_s(int x, int y, const char *s, int sc, u8 r, u8 g, u8 b)
{
	const u8 *font = consoleGetDefault()->font.gfx;
	for (; *s; s++, x += 6 * sc) {
		const u8 *ch = font + (u8)*s * 8;
		for (int row = 0; row < 8; row++)
			for (int col = 0; col < 8; col++)
				if (ch[row] & (0x80 >> col)) {
					if (sc == 1) putpx(x + col, y + row, r, g, b);
					else fill_rect(x + col * sc, y + row * sc, sc, sc, r, g, b);
				}
	}
}

static void draw_text(int x, int y, const char *s, u8 r, u8 g, u8 b)
{
	draw_text_s(x, y, s, 1, r, g, b);
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
/* icons                                                               */
/* ------------------------------------------------------------------ */

/* Manoeuvre arrow in a (22*s)-pixel box at (x, y). */
static void draw_arrow_s(int x, int y, int s, char kind, u8 r, u8 g, u8 b)
{
#define L(x0, y0, x1, y1) draw_line(x + (x0) * s, y + (y0) * s, x + (x1) * s, y + (y1) * s, s, r, g, b)
	switch (kind) {
	case 'L': case 'l':
		L(15, 20, 15, 9); L(15, 9, 4, 9); L(4, 9, 9, 4); L(4, 9, 9, 14);
		break;
	case 'R': case 'r':
		L(7, 20, 7, 9); L(7, 9, 18, 9); L(18, 9, 13, 4); L(18, 9, 13, 14);
		break;
	case 'U':
		L(16, 20, 16, 7); L(16, 7, 6, 7); L(6, 7, 6, 16); L(6, 16, 2, 12); L(6, 16, 10, 12);
		break;
	case 'O':
		for (int a = 0; a < 360; a += 15) {
			double t = a * M_PI / 180;
			fill_circle(x + 11 * s + (int)(6 * s * cos(t)), y + 10 * s + (int)(6 * s * sin(t)), s, r, g, b);
		}
		L(11, 21, 11, 16);
		break;
	case 'A':
		fill_circle(x + 11 * s, y + 8 * s, 6 * s, r, g, b);
		L(11, 8, 11, 21);
		fill_circle(x + 11 * s, y + 8 * s, 2 * s, 40, 40, 40);
		break;
	default:
		L(11, 21, 11, 3); L(11, 3, 6, 8); L(11, 3, 16, 8);
	}
#undef L
}

static void draw_arrow(int x, int y, char kind, u8 r, u8 g, u8 b)
{
	draw_arrow_s(x, y, 1, kind, r, g, b);
}

static void icon_search(int x, int y, u8 r, u8 g, u8 b)
{
	for (int a = 0; a < 360; a += 8) {
		double t = a * M_PI / 180;
		fill_circle(x + 9 + (int)(7 * cos(t)), y + 9 + (int)(7 * sin(t)), 1, r, g, b);
	}
	draw_line(x + 14, y + 14, x + 20, y + 20, 2, r, g, b);
}

static void icon_car(int x, int y, u8 r, u8 g, u8 b)
{
	/* 26 x 18 */
	fill_rect(x + 6, y + 1, 14, 7, r, g, b);
	fill_rect(x, y + 7, 26, 7, r, g, b);
	fill_circle(x + 6, y + 15, 3, r, g, b);
	fill_circle(x + 20, y + 15, 3, r, g, b);
}

static void icon_walk(int x, int y, u8 r, u8 g, u8 b)
{
	/* 16 x 20 */
	fill_circle(x + 9, y + 2, 2, r, g, b);
	draw_line(x + 8, y + 6, x + 6, y + 12, 1, r, g, b);     /* body */
	draw_line(x + 6, y + 12, x + 2, y + 19, 1, r, g, b);    /* back leg */
	draw_line(x + 6, y + 12, x + 11, y + 19, 1, r, g, b);   /* front leg */
	draw_line(x + 8, y + 7, x + 13, y + 10, 1, r, g, b);    /* arm */
	draw_line(x + 7, y + 7, x + 2, y + 11, 1, r, g, b);     /* arm */
}

static void icon_target(int x, int y, u8 r, u8 g, u8 b)
{
	for (int a = 0; a < 360; a += 10) {
		double t = a * M_PI / 180;
		putpx(x + 9 + (int)(7 * cos(t)), y + 9 + (int)(7 * sin(t)), r, g, b);
		putpx(x + 9 + (int)(6 * cos(t)), y + 9 + (int)(6 * sin(t)), r, g, b);
	}
	fill_circle(x + 9, y + 9, 2, r, g, b);
	draw_line(x + 9, y, x + 9, y + 2, 0, r, g, b);
	draw_line(x + 9, y + 16, x + 9, y + 18, 0, r, g, b);
	draw_line(x, y + 9, x + 2, y + 9, 0, r, g, b);
	draw_line(x + 16, y + 9, x + 18, y + 9, 0, r, g, b);
}

/* ------------------------------------------------------------------ */
/* route on the map                                                    */
/* ------------------------------------------------------------------ */
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
/* bottom screen: touch UI                                             */
/* ------------------------------------------------------------------ */

typedef struct { u8 r, g, b; } Col;
static const Col C_BG = {16, 20, 28}, C_CARD = {36, 42, 56}, C_SEL = {44, 70, 125},
                 C_ACC = {59, 130, 246}, C_GO = {30, 160, 85}, C_TXT = {242, 244, 248},
                 C_SUB = {150, 160, 178}, C_WARN = {245, 185, 60}, C_RED = {205, 65, 65},
                 C_NAV = {22, 105, 70};

static bool touch_now;          /* screen touched this frame (not held) */
static int touch_x, touch_y;

static void rrect(int x, int y, int w, int h, int rad, Col c)
{
	fill_rect(x + rad, y, w - 2 * rad, h, c.r, c.g, c.b);
	fill_rect(x, y + rad, rad, h - 2 * rad, c.r, c.g, c.b);
	fill_rect(x + w - rad, y + rad, rad, h - 2 * rad, c.r, c.g, c.b);
	fill_circle(x + rad, y + rad, rad, c.r, c.g, c.b);
	fill_circle(x + w - rad - 1, y + rad, rad, c.r, c.g, c.b);
	fill_circle(x + rad, y + h - rad - 1, rad, c.r, c.g, c.b);
	fill_circle(x + w - rad - 1, y + h - rad - 1, rad, c.r, c.g, c.b);
}

static void text(int x, int y, const char *s, int sc, Col c) { draw_text_s(x, y, s, sc, c.r, c.g, c.b); }
static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc; }

/* draw at most max_chars characters, ending with ".." if cut */
static void text_fit(int x, int y, const char *s, int sc, int max_px, Col c)
{
	char buf[128];
	int max = max_px / (6 * sc);
	if (max > 127) max = 127;
	snprintf(buf, sizeof(buf), "%s", s);
	if ((int)strlen(buf) > max && max > 2) { buf[max - 2] = '.'; buf[max - 1] = '.'; buf[max] = 0; }
	text(x, y, buf, sc, c);
}

static void text_center(int x, int y, int w, const char *s, int sc, Col c)
{
	text(x + (w - text_w(s, sc)) / 2, y, s, sc, c);
}

/* word-wrap into up to `maxl` lines of `width` characters; returns lines used */
static int wrap(const char *s, int width, char out[][64], int maxl)
{
	int l = 0;
	while (*s && l < maxl) {
		int n = strlen(s);
		if (n > width) {
			n = width;
			while (n > 0 && s[n] != ' ') n--;
			if (n == 0) n = width;
		}
		if (l == maxl - 1 && (int)strlen(s) > width) {   /* last line: cut with .. */
			n = width;
			snprintf(out[l], 64, "%.*s..", width - 2, s);
		} else snprintf(out[l], 64, "%.*s", n, s);
		l++;
		s += n;
		while (*s == ' ') s++;
	}
	return l;
}

static bool hit(int x, int y, int w, int h)
{
	return touch_now && touch_x >= x && touch_x < x + w && touch_y >= y && touch_y < y + h;
}

static bool button(int x, int y, int w, int h, const char *label, int sc, Col bg, Col fg)
{
	bool pressed = hit(x, y, w, h);
	if (pressed) { bg.r = bg.r / 2 + 100; bg.g = bg.g / 2 + 100; bg.b = bg.b / 2 + 100; }
	rrect(x, y, w, h, 6, bg);
	if (label) text_center(x, y + (h - 8 * sc) / 2, w, label, sc, fg);
	return pressed;
}

/* Car / Walk switch. Returns true if it changed. */
static bool mode_switch(int x, int y, int w, int h, bool *walking, bool big)
{
	int half = w / 2 - 2;
	bool changed = false;
	if (hit(x, y, half, h) && *walking) { *walking = false; changed = true; }
	if (hit(x + half + 4, y, half, h) && !*walking) { *walking = true; changed = true; }
	Col car = *walking ? C_CARD : C_ACC, walk = *walking ? C_ACC : C_CARD;
	rrect(x, y, half, h, 6, car);
	rrect(x + half + 4, y, half, h, 6, walk);
	int sc = big ? 2 : 1;
	int cw = 26 + 6 + text_w("Car", sc), ww = 16 + 6 + text_w("Walk", sc);
	int cx = x + (half - cw) / 2, wx = x + half + 4 + (half - ww) / 2;
	icon_car(cx, y + (h - 18) / 2, C_TXT.r, C_TXT.g, C_TXT.b);
	text(cx + 32, y + (h - 8 * sc) / 2, "Car", sc, C_TXT);
	icon_walk(wx, y + (h - 20) / 2, C_TXT.r, C_TXT.g, C_TXT.b);
	text(wx + 22, y + (h - 8 * sc) / 2, "Walk", sc, C_TXT);
	return changed;
}

static void status_chips(const GpsNet *g, int x, int y)
{
	u64 now = osGetTime();
	bool fix = g->fix.valid && g->last_fix_ms && now - g->last_fix_ms < 5000;
	Col dot = fix ? C_GO : g->last_rx_ms ? C_WARN : C_RED;
	fill_circle(x + 4, y + 4, 4, dot.r, dot.g, dot.b);
	text(x + 12, y, fix ? "GPS" : g->last_rx_ms ? "No fix" : "No phone", 1, C_SUB);
	int tx = x + 12 + text_w(fix ? "GPS" : g->last_rx_ms ? "No fix" : "No phone", 1) + 10;
	Col td = tiles_online() ? C_GO : C_WARN;
	fill_circle(tx + 4, y + 4, 4, td.r, td.g, td.b);
	text(tx + 12, y, tiles_online() ? "Map" : "Offline", 1, C_SUB);
}

static void header(const char *title, bool back, bool *back_pressed)
{
	fill_rect(0, 0, 320, 30, 26, 31, 42);
	if (back) {
		*back_pressed = button(4, 3, 64, 24, "< Back", 1, C_CARD, C_TXT);
		text(78, 8, title, 2, C_TXT);
	} else text(10, 8, title, 2, C_TXT);
}

static void footer_msg(const char *msg, int y)
{
	if (msg && *msg) {
		rrect(6, y, 308, 16, 4, (Col){70, 56, 20});
		text_fit(12, y + 4, msg, 1, 296, C_WARN);
	}
}

typedef enum { UI_HOME, UI_RESULTS, UI_INFO } UiMode;

typedef enum {
	ACT_NONE, ACT_SEARCH, ACT_FOLLOW, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_INFO, ACT_BACK,
	ACT_GO, ACT_END_ROUTE, ACT_MODE_CHANGED, ACT_TOGGLE_TILES, ACT_SELECT, ACT_PHONE_IP
} Action;

static Action screen_home(const GpsNet *g, bool *walking, bool follow, const char *msg)
{
	Action a = ACT_NONE;
	bool dummy;
	header("3DS Maps", false, &dummy);
	status_chips(g, 180, 11);

	/* big "Where to?" */
	bool p = hit(10, 40, 300, 56);
	rrect(10, 40, 300, 56, 10, p ? (Col){110, 160, 240} : C_ACC);
	icon_search(28, 57, 255, 255, 255);
	text(62, 60, "Where to?", 2, C_TXT);
	if (p) a = ACT_SEARCH;

	text(12, 106, "Travel by", 1, C_SUB);
	if (mode_switch(10, 118, 300, 44, walking, true)) a = ACT_MODE_CHANGED;

	/* map controls */
	bool fp = hit(10, 172, 110, 40);
	rrect(10, 172, 110, 40, 6, fp ? C_SEL : (follow ? C_SEL : C_CARD));
	icon_target(18, 183, C_TXT.r, C_TXT.g, C_TXT.b);
	text(42, 188, "Follow me", 1, C_TXT);
	if (fp) a = ACT_FOLLOW;
	if (button(126, 172, 44, 40, "-", 3, C_CARD, C_TXT)) a = ACT_ZOOM_OUT;
	if (button(176, 172, 44, 40, "+", 3, C_CARD, C_TXT)) a = ACT_ZOOM_IN;
	if (button(226, 172, 84, 40, "Info", 2, C_CARD, C_TXT)) a = ACT_INFO;

	if (msg && *msg) footer_msg(msg, 220);
	else text(10, 226, "Map data (c) OpenStreetMap contributors", 1, (Col){95, 102, 118});
	return a;
}

static int results_top;   /* first visible card */

static Action screen_results(const Place *pl, int n, int *sel, bool *walking, const char *query,
                             const char *msg, bool busy)
{
	Action a = ACT_NONE;
	bool back = false;
	char title[40];
	snprintf(title, sizeof(title), "\"%.15s\"", query);
	header(title, true, &back);
	if (back) a = ACT_BACK;

	if (n == 0) {
		text_center(0, 90, 320, "No places found.", 2, C_TXT);
		text_center(0, 116, 320, "Try another name or add the city.", 1, C_SUB);
		if (button(80, 140, 160, 36, "Search again", 1, C_ACC, C_TXT)) a = ACT_SEARCH;
		return a;
	}

	if (*sel < results_top) results_top = *sel;
	if (*sel > results_top + 2) results_top = *sel - 2;
	for (int i = 0; i < 3 && results_top + i < n; i++) {
		int idx = results_top + i, y = 36 + i * 48;
		bool s = idx == *sel;
		if (hit(6, y, 276, 44)) { *sel = idx; a = ACT_SELECT; s = true; }
		rrect(6, y, 276, 44, 6, s ? C_SEL : C_CARD);
		char num[12];
		snprintf(num, sizeof(num), "%d", idx + 1);
		fill_circle(22, y + 22, 10, s ? C_ACC.r : 70, s ? C_ACC.g : 78, s ? C_ACC.b : 96);
		text_center(12, y + 18, 21, num, 1, C_TXT);
		text_fit(38, y + 8, pl[idx].name, 2, 238, C_TXT);
		text_fit(38, y + 30, pl[idx].detail, 1, 238, C_SUB);
	}
	/* scroll buttons */
	bool can_up = results_top > 0, can_dn = results_top + 3 < n;
	if (button(286, 36, 28, 68, NULL, 1, can_up ? C_CARD : C_BG, C_TXT) && can_up) {
		results_top--; if (*sel > results_top + 2) *sel = results_top + 2;
	}
	if (button(286, 108, 28, 72, NULL, 1, can_dn ? C_CARD : C_BG, C_TXT) && can_dn) {
		results_top++; if (*sel < results_top) *sel = results_top;
	}
	for (int i = 0; i < 7; i++) {   /* triangles */
		Col u = can_up ? C_TXT : C_CARD, d = can_dn ? C_TXT : C_CARD;
		fill_rect(300 - i, 64 + i, 2 * i + 1, 1, u.r, u.g, u.b);
		fill_rect(300 - i, 150 - i, 2 * i + 1, 1, d.r, d.g, d.b);
	}

	if (msg && *msg) footer_msg(msg, 182);
	if (mode_switch(6, 200, 156, 36, walking, false)) a = ACT_MODE_CHANGED;
	bool gp = hit(168, 200, 146, 36);
	rrect(168, 200, 146, 36, 8, busy ? C_CARD : gp ? (Col){80, 200, 130} : C_GO);
	text_center(168, 210, 146, busy ? "Wait..." : "Go  >", 2, C_TXT);
	if (gp && !busy) a = ACT_GO;
	return a;
}

static Action screen_nav(const GpsNet *g, const Route *r, const NavProgress *p, const Place *dest,
                         bool *walking, bool have_pos, bool rerouting, const char *msg)
{
	Action a = ACT_NONE;
	char lines[3][64], d[24];

	/* current instruction */
	fill_rect(0, 0, 320, 100, C_NAV.r, C_NAV.g, C_NAV.b);
	const RouteStep *st = &r->steps[have_pos ? p->next_step : 0];
	if (have_pos && p->arrived) {
		draw_arrow_s(8, 14, 3, 'A', 255, 255, 255);
		text(86, 22, "You have", 2, C_TXT);
		text(86, 44, "arrived!", 3, C_TXT);
	} else {
		draw_arrow_s(8, 14, 3, st->arrow, 255, 255, 255);
		if (have_pos) format_distance(d, sizeof(d), p->to_next_m);
		else snprintf(d, sizeof(d), "Start");
		text(86, 10, d, 3, C_TXT);
		int nl = wrap(st->text, 19, lines, 2);
		bool small = (int)strlen(st->text) > 38;
		if (small) {
			nl = wrap(st->text, 38, lines, 3);
			for (int i = 0; i < nl; i++) text(86, 44 + i * 12, lines[i], 1, C_TXT);
		} else
			for (int i = 0; i < nl; i++) text(86, 44 + i * 20, lines[i], 2, C_TXT);
	}

	/* next step */
	fill_rect(0, 100, 320, 28, 28, 34, 46);
	if (have_pos && !p->arrived && p->next_step + 1 < r->nsteps) {
		const RouteStep *nx = &r->steps[p->next_step + 1];
		draw_arrow_s(8, 103, 1, nx->arrow, C_SUB.r, C_SUB.g, C_SUB.b);
		text(34, 110, "Then", 1, C_SUB);
		text_fit(64, 110, nx->text, 1, 250, C_TXT);
	} else {
		text(10, 110, "To:", 1, C_SUB);
		text_fit(34, 110, dest->name, 1, 280, C_TXT);
	}

	/* distance, time, arrival */
	double rem_m = have_pos ? p->remaining_m : r->distance;
	double rem_s = have_pos ? p->remaining_s : r->duration;
	char dist[24], mins[24], eta[24];
	format_distance(dist, sizeof(dist), rem_m);
	int m = (int)(rem_s / 60.0 + 0.5);
	if (m >= 60) snprintf(mins, sizeof(mins), "%dh%02d", m / 60, m % 60);
	else snprintf(mins, sizeof(mins), "%d min", m);
	time_t t = time(NULL) + (time_t)rem_s;
	struct tm *tm = gmtime(&t);   /* the 3DS clock is local time */
	snprintf(eta, sizeof(eta), "%02d:%02d", tm->tm_hour, tm->tm_min);
	text_center(0, 138, 107, dist, 2, C_TXT);
	text_center(107, 138, 106, mins, 2, C_TXT);
	text_center(213, 138, 107, eta, 2, C_TXT);
	text_center(0, 158, 107, "left", 1, C_SUB);
	text_center(107, 158, 106, *walking ? "walking" : "driving", 1, C_SUB);
	text_center(213, 158, 107, "arrival", 1, C_SUB);

	if (msg && *msg) footer_msg(msg, 174);
	else if (rerouting) footer_msg("Finding a new route...", 174);
	else if (!have_pos) footer_msg("No GPS yet: route starts at the map centre", 174);
	else if (p->off_route_m > 30 && !p->arrived) {
		char o[48];
		snprintf(o, sizeof(o), "Off route by %d m", (int)p->off_route_m);
		footer_msg(o, 174);
	} else {
		status_chips(g, 10, 178);
	}

	if (button(6, 196, 96, 40, "End", 2, C_RED, C_TXT)) a = ACT_END_ROUTE;
	bool mp = hit(108, 196, 104, 40);
	rrect(108, 196, 104, 40, 6, mp ? C_SEL : C_CARD);
	if (*walking) { icon_walk(122, 206, 255, 255, 255); text(146, 212, "Walk", 2, C_TXT); }
	else { icon_car(118, 207, 255, 255, 255); text(150, 212, "Car", 2, C_TXT); }
	if (mp) { *walking = !*walking; a = ACT_MODE_CHANGED; }
	bool sp = hit(218, 196, 96, 40);
	rrect(218, 196, 96, 40, 6, sp ? C_SEL : C_CARD);
	icon_search(226, 205, 255, 255, 255);
	text(252, 212, "New", 2, C_TXT);
	if (sp) a = ACT_SEARCH;
	return a;
}

static const char *tcp_state_str(TcpState s)
{
	switch (s) {
	case TCP_OFF: return "off";
	case TCP_CONNECTING: return "trying";
	case TCP_CONNECTED: return "connected";
	default: return "waiting";
	}
}

static void info_row(int y, const char *label, const char *value, Col vc)
{
	text(10, y, label, 1, C_SUB);
	text_fit(92, y, value, 1, 222, vc);
}

static Action screen_info(const GpsNet *g, const Config *cfg, int z, double clat, double clon)
{
	Action a = ACT_NONE;
	bool back = false;
	char v[96];
	u64 now = osGetTime();
	const GpsFix *f = &g->fix;
	header("Info", true, &back);
	if (back) a = ACT_BACK;

	int y = 38;
	rrect(4, y - 4, 312, 74, 6, C_CARD);
	if (!g->last_rx_ms) info_row(y, "GPS", "waiting for phone...", C_WARN);
	else if (f->valid && now - g->last_fix_ms < 5000) {
		snprintf(v, sizeof(v), "FIX (data %us ago)", (unsigned)((now - g->last_rx_ms) / 1000));
		info_row(y, "GPS", v, (Col){90, 220, 130});
	} else if (g->last_fix_ms) {
		snprintf(v, sizeof(v), "LOST (last fix %us ago)", (unsigned)((now - g->last_fix_ms) / 1000));
		info_row(y, "GPS", v, C_WARN);
	} else info_row(y, "GPS", "no fix yet (phone sending)", C_WARN);
	snprintf(v, sizeof(v), "%.5f, %.5f", f->lat, f->lon);
	info_row(y + 13, "Position", v, C_TXT);
	snprintf(v, sizeof(v), "%.1f km/h, course %.0f", f->speed_kmh, f->course_deg < 0 ? 0 : f->course_deg);
	info_row(y + 26, "Speed", v, C_TXT);
	snprintf(v, sizeof(v), "sats %d, HDOP %.1f, alt %.0f m", f->sats, f->hdop, f->altitude_m);
	info_row(y + 39, "Signal", v, C_TXT);
	info_row(y + 52, "From", g->source, C_TXT);

	y = 118;
	rrect(4, y - 4, 312, 48, 6, C_CARD);
	snprintf(v, sizeof(v), "%s  (UDP %d)", g->my_ip, cfg->udp_port);
	info_row(y, "3DS IP", v, C_TXT);
	if (g->tcp_state != TCP_OFF) snprintf(v, sizeof(v), "%s %s", g->tcp_target, tcp_state_str(g->tcp_state));
	else snprintf(v, sizeof(v), "off");
	info_row(y + 13, "Phone", v, g->tcp_state == TCP_CONNECTED ? (Col){90, 220, 130} : C_TXT);
	if (g->web_port) snprintf(v, sizeof(v), "http://%s:%d", g->my_ip, g->web_port);
	else snprintf(v, sizeof(v), "off");
	info_row(y + 26, "Phone page", v, C_TXT);

	y = 172;
	rrect(4, y - 4, 312, 22, 6, C_CARD);
	snprintf(v, sizeof(v), "z%d %.4f,%.4f  load %d  dl %d", z, clat, clon, tiles_pending(), tiles_downloaded());
	info_row(y, "Map", v, C_TXT);
	if (tiles_last_error()[0]) text_fit(10, 194, tiles_last_error(), 1, 300, C_WARN);

	if (button(6, 204, 100, 32, tiles_online() ? "Tiles: ON" : "Tiles: OFF", 1,
	           tiles_online() ? C_GO : C_WARN, C_TXT)) a = ACT_TOGGLE_TILES;
	if (button(110, 204, 100, 32, "Phone IP", 1, C_ACC, C_TXT)) a = ACT_PHONE_IP;
	if (button(214, 204, 100, 32, "Back", 1, C_CARD, C_TXT)) a = ACT_BACK;
	return a;
}

/* ------------------------------------------------------------------ */

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

/* Phone address: "auto", an IP like 192.168.43.1, or "off". */
static bool ask_phone_ip(char *out, int n)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, n - 1);
	swkbdSetHintText(&kb, "Phone IP, or auto");
	swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	swkbdSetInitialText(&kb, out);
	char buf[64] = {0};
	if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_RIGHT) return false;
	char *s = buf;
	while (*s == ' ') s++;
	char *e = s + strlen(s);
	while (e > s && e[-1] == ' ') *--e = 0;
	for (char *c = s; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
	if (!*s) return false;
	struct in_addr a;
	if (strcmp(s, "auto") && strcmp(s, "off") && !inet_aton(s, &a)) return false;
	snprintf(out, n, "%s", s);
	return true;
}

int main(void)
{
	osSetSpeedupEnable(true);   /* New 3DS: 804 MHz + L2 cache */
	gfxInitDefault();
	gfxSet3D(false);
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

	/* search / navigation state */
	UiMode ui = UI_HOME;
	char query[128] = "", msg[96] = "";
	u64 msg_until = 0;
	Place places[MAX_PLACES];
	int nplaces = 0, sel = 0;
	Route route = {0};
	bool have_route = false, walking = false, rerouting = false;
	Place dest = {0};
	NavProgress prog = {0};
	u64 off_since = 0, last_route_req = 0;

#define SAY(...) do { snprintf(msg, sizeof(msg), __VA_ARGS__); msg_until = osGetTime() + 6000; } while (0)

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;
		touch_now = false;
		if (down & KEY_TOUCH) {
			touchPosition t;
			hidTouchRead(&t);
			touch_now = true;
			touch_x = t.px;
			touch_y = t.py;
		}

		if (net_ok) gps_net_poll(&gps);
		bool have_fix = gps.fix.valid && gps.last_fix_ms && osGetTime() - gps.last_fix_ms < 30000;
		u64 now = osGetTime();
		if (msg[0] && msg_until && now > msg_until && navnet_state() != JOB_RUNNING) msg[0] = 0;

		/* ---- finished network requests ---- */
		JobState js = navnet_state();
		if (js == JOB_FAILED) {
			SAY("%s", navnet_error());
			rerouting = false;
			navnet_clear();
		} else if (js == JOB_DONE) {
			if (navnet_kind() == JOB_SEARCH) {
				nplaces = nav_parse_search(navnet_body(), places, MAX_PLACES);
				if (nplaces < 0) { nplaces = 0; SAY("Search failed: bad reply"); }
				else msg[0] = 0;
				sel = 0;
				results_top = 0;
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
					if (!rerouting) { follow = true; ui = UI_HOME; }
				} else {
					SAY("Route failed: %s", err);
				}
				rerouting = false;
			}
			navnet_clear();
		}

		/* ---- buttons (touch actions come from the previous frame's screen) ---- */
		static Action pending = ACT_NONE;
		Action act = pending;
		pending = ACT_NONE;
		if (down & KEY_X) act = ACT_SEARCH;
		if (down & KEY_SELECT) act = ACT_TOGGLE_TILES;
		if (ui == UI_RESULTS) {
			if ((down & KEY_DOWN) && sel + 1 < nplaces) { sel++; act = ACT_SELECT; }
			if ((down & KEY_UP) && sel > 0) { sel--; act = ACT_SELECT; }
			if (down & KEY_B) act = ACT_BACK;
			if (down & KEY_A) act = ACT_GO;
			if (down & KEY_Y) { walking = !walking; act = ACT_MODE_CHANGED; }
		} else if (ui == UI_INFO) {
			if (down & KEY_B) act = ACT_BACK;
		}

		switch (act) {
		case ACT_SEARCH:
			if (!net_ok) SAY("Search needs Wi-Fi (connect to the phone hotspot)");
			else if (navnet_state() == JOB_RUNNING) SAY("Please wait...");
			else if (ask_search_text(query, sizeof(query), query)) {
				double slat = have_fix ? gps.fix.lat : px_to_lat(cy, z);
				double slon = have_fix ? gps.fix.lon : px_to_lon(cx, z);
				navnet_search(query, slat, slon);
				snprintf(msg, sizeof(msg), "Searching for \"%.50s\"...", query);
				msg_until = 0;
			}
			break;
		case ACT_GO:
			if (ui == UI_RESULTS && nplaces > 0 && navnet_state() == JOB_IDLE) {
				dest = places[sel];
				double flat = have_fix ? gps.fix.lat : px_to_lat(cy, z);
				double flon = have_fix ? gps.fix.lon : px_to_lon(cx, z);
				navnet_route(flat, flon, dest.lat, dest.lon, walking);
				last_route_req = now;
				snprintf(msg, sizeof(msg), "Getting %s directions...", walking ? "walking" : "driving");
				msg_until = 0;
			}
			break;
		case ACT_MODE_CHANGED:
			/* switching car/walk during a route: get the route again */
			if (have_route && ui == UI_HOME && navnet_state() == JOB_IDLE) {
				double flat = have_fix ? gps.fix.lat : route.lat[0];
				double flon = have_fix ? gps.fix.lon : route.lon[0];
				navnet_route(flat, flon, dest.lat, dest.lon, walking);
				last_route_req = now;
				rerouting = true;
			}
			break;
		case ACT_SELECT:
			if (ui == UI_RESULTS && nplaces > 0) {   /* show the chosen place on the map */
				follow = false;
				cx = lon_to_px(places[sel].lon, z);
				cy = lat_to_px(places[sel].lat, z);
			}
			break;
		case ACT_BACK: ui = UI_HOME; msg[0] = 0; break;
		case ACT_INFO: ui = UI_INFO; break;
		case ACT_FOLLOW: follow = true; break;
		case ACT_TOGGLE_TILES: tiles_set_online(!tiles_online()); break;
		case ACT_PHONE_IP:
			if (ask_phone_ip(cfg.phone_ip, sizeof(cfg.phone_ip))) {
				gps_net_set_phone(&gps, cfg.phone_ip);
				config_save(&cfg, APP_DIR "/config.ini");
				SAY("Phone: %s", cfg.phone_ip);
			}
			break;
		case ACT_END_ROUTE:
			if (have_route) { route_free(&route); have_route = false; msg[0] = 0; }
			break;
		case ACT_ZOOM_IN: case ACT_ZOOM_OUT: break;   /* handled below */
		default: break;
		}

		/* ---- map movement ---- */
		int nz = z;
		if (down & (KEY_R | KEY_ZR) || act == ACT_ZOOM_IN) nz = z < MAX_ZOOM ? z + 1 : z;
		if (down & (KEY_L | KEY_ZL) || act == ACT_ZOOM_OUT) nz = z > MIN_ZOOM ? z - 1 : z;
		if (nz != z) {
			double f = nz > z ? 2.0 : 0.5;
			cx *= f; cy *= f; z = nz;
		}
		if (ui != UI_RESULTS) {
			circlePosition cp;
			hidCircleRead(&cp);
			double dx = 0, dy = 0;
			if (abs(cp.dx) > 15) dx += cp.dx / 16.0;
			if (abs(cp.dy) > 15) dy -= cp.dy / 16.0;
			if (held & KEY_DLEFT) dx -= 4;
			if (held & KEY_DRIGHT) dx += 4;
			if (held & KEY_DUP) dy -= 4;
			if (held & KEY_DDOWN) dy += 4;
			if (dx != 0 || dy != 0) { cx += dx; cy += dy; follow = false; }
			if (down & KEY_A) follow = true;
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
		use_screen(GFX_TOP);
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
				if (i == sel) continue;
				fill_circle(px, py, 7, 255, 255, 255);
				fill_circle(px, py, 6, 200, 60, 60);
				char n[12];
				snprintf(n, sizeof(n), "%d", i + 1);
				draw_text(px - 2, py - 3, n, 255, 255, 255);
			}
			if (nplaces > 0)
				draw_pin((int)(lon_to_px(places[sel].lon, z) - wx), (int)(lat_to_px(places[sel].lat, z) - wy));
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

		if (have_route && have_fix && ui == UI_HOME) {
			/* small instruction banner on the map too */
			char line[80], d[24];
			const RouteStep *st = &route.steps[prog.next_step];
			fill_rect(0, 0, SCR_W, 26, 22, 105, 70);
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
			char lbl[64];
			snprintf(lbl, sizeof(lbl), "z%d%s%s", z, follow && have_fix ? " follow" : "",
			         tiles_online() ? "" : "  OFFLINE - press SELECT to load map");
			draw_label(2, 2, lbl);
		}
		draw_label(SCR_W - 6 * 26 - 6, SCR_H - 13, "(c) OpenStreetMap contrib.");

		/* ---- draw bottom screen ---- */
		use_screen(GFX_BOTTOM);
		fill_rect(0, 0, 320, 240, C_BG.r, C_BG.g, C_BG.b);
		bool busy = navnet_state() == JOB_RUNNING;
		if (ui == UI_RESULTS) pending = screen_results(places, nplaces, &sel, &walking, query, msg, busy);
		else if (ui == UI_INFO) pending = screen_info(&gps, &cfg, z, px_to_lat(cy, z), px_to_lon(cx, z));
		else if (have_route) pending = screen_nav(&gps, &route, &prog, &dest, &walking, have_fix, rerouting, msg);
		else pending = screen_home(&gps, &walking, follow, msg);
		/* button presses on the results list re-centre right away; others next frame */

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
