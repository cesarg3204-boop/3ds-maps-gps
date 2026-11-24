/* Renders the 3DS Maps screens to PNG on a PC, to check the UI layout. */
#define main app_main
#include "../../source/main.c"
#undef main
#include <stdio.h>

static u8 font_data[2048];
static PrintConsole con = { { font_data, 0, 256 } };
PrintConsole *consoleGetDefault(void) { return &con; }
static u8 top_fb[400 * 240 * 3], bot_fb[320 * 240 * 3];
u8 *gfxGetFramebuffer(gfxScreen_t s, gfx3dSide_t side, u16 *w, u16 *h) { (void)side;(void)w;(void)h; return s == GFX_TOP ? top_fb : bot_fb; }
static u64 fake_now = 100000;
u64 osGetTime(void) { return fake_now; }

/* tiles: flat "map" colour */
static u8 land[256 * 256 * 3];
const uint8_t *tiles_get(int z, int x, int y, bool *m) { (void)z;(void)x;(void)y; *m = false; return land; }
const uint8_t *tiles_peek(int z, int x, int y) { (void)z;(void)x;(void)y; return land; }
void tiles_prefetch(int z, int x, int y) { (void)z;(void)x;(void)y; }
bool tiles_begin_frame(void) { return false; }
static bool online = true;
void tiles_set_online(bool on) { online = on; }
bool tiles_online(void) { return online; }
int tiles_pending(void) { return 2; }
int tiles_downloaded(void) { return 37; }
const char *tiles_last_error(void) { return ""; }
void tiles_init(const Config *c) { (void)c; }
void tiles_exit(void) {}
JobState navnet_state(void) { return JOB_IDLE; }

static void save(const char *name, u8 *fbp, int w)
{
	char path[256];
	snprintf(path, sizeof(path), "out/%s.ppm", name);
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6 %d 240 255\n", w);
	for (int y = 0; y < 240; y++)
		for (int x = 0; x < w; x++) {
			u8 *p = fbp + (x * 240 + (239 - y)) * 3;
			u8 rgb[3] = { p[2], p[1], p[0] };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f); return b; }

static void bottom_bg(void) { use_screen(GFX_BOTTOM); fill_rect(0, 0, 320, 240, C_BG.r, C_BG.g, C_BG.b); }

int main(void)
{
	FILE *ff = fopen("font.bin", "rb");   /* libctru default_font.bin */
	fread(font_data, 1, 2048, ff); fclose(ff);
	for (int i = 0; i < 256 * 256; i++) { land[i*3] = 0xE9; land[i*3+1] = 0xEF; land[i*3+2] = 0xF2; }

	GpsNet g; memset(&g, 0, sizeof(g)); nmea_init(&g.fix);
	strcpy(g.my_ip, "192.168.68.69"); strcpy(g.tcp_target, "127.0.0.1:11123"); g.tcp_state = TCP_CONNECTED;
	g.web_port = 8080; strcpy(g.source, "TCP");
	g.fix.valid = true; g.fix.lat = 8.98638; g.fix.lon = -79.50241; g.fix.sats = 9; g.fix.hdop = 0.9;
	g.last_rx_ms = g.last_fix_ms = fake_now - 300;
	Config cfg; memset(&cfg, 0, sizeof(cfg)); cfg.udp_port = 10110;
	bool walking = false;

	bottom_bg(); screen_home(&g, &walking, true, ""); save("home", bot_fb, 320);
	walking = true; touch_now = false;
	bottom_bg(); screen_home(&g, &walking, false, "Searching for \"multiplaza\"..."); save("home_walk_msg", bot_fb, 320);

	Place pl[MAX_PLACES]; char *js = slurp("../fixtures/search_prado.json"); int n = nav_parse_search(js, pl, MAX_PLACES);
	strcpy(pl[2].name, "Multiplaza Pacific"); strcpy(pl[2].detail, "Avenida Israel, Villa Lilla, San Francisco, Panama"); pl[2].lat = 8.986; pl[2].lon = -79.51;
	strcpy(pl[3].name, "Multiplaza Mall Parking"); strcpy(pl[3].detail, "Calle 77 Este, San Francisco, Panama"); pl[3].lat = 8.987; pl[3].lon = -79.505;
	n = 4; int sel = 1; walking = false;
	bottom_bg(); screen_results(pl, n, &sel, &walking, "multiplaza", "", false); save("results", bot_fb, 320);
	bottom_bg(); screen_results(pl, 0, &sel, &walking, "xyzzy", "", false); save("results_none", bot_fb, 320);

	Route r; char err[64]; js = slurp("../fixtures/route_foot_madrid.json");
	if (!nav_parse_route(js, &r, err, sizeof(err))) { printf("route: %s\n", err); return 1; }
	NavProgress p = {0}; nav_progress(&r, 40.407368, -3.697974, &p);
	Place dest = pl[0];
	bottom_bg(); screen_nav(&g, &r, &p, &dest, &walking, true, false, ""); save("nav", bot_fb, 320);
	p.seg = 0; nav_progress(&r, 40.4080, -3.6965, &p);
	walking = true;
	bottom_bg(); screen_nav(&g, &r, &p, &dest, &walking, true, false, ""); save("nav2", bot_fb, 320);
	nav_progress(&r, 40.413885, -3.691771, &p);
	bottom_bg(); screen_nav(&g, &r, &p, &dest, &walking, true, false, ""); save("nav_arrived", bot_fb, 320);
	bottom_bg(); screen_info(&g, &cfg, 16, 8.98638, -79.50241); save("info", bot_fb, 320);

	/* top screen with route */
	use_screen(GFX_TOP);
	int z = 16; double cx = lon_to_px(-3.6955, z), cy = lat_to_px(40.4095, z);
	long wx = (long)cx - 200, wy = (long)cy - 120;
	draw_map(fb, z, wx, wy);
	draw_route(&r, z, wx, wy, 5);
	draw_pin((int)(lon_to_px(r.lon[r.npts-1], z) - wx), (int)(lat_to_px(r.lat[r.npts-1], z) - wy));
	draw_position((int)(lon_to_px(-3.6965, z) - wx), (int)(lat_to_px(40.4080, z) - wy), 45, true, false);
	save("top_route", top_fb, 400);
	printf("ok\n");
	return 0;
}
void config_load(Config *c, const char *p) { (void)c;(void)p; }
bool state_load(double *a, double *b, int *z) { (void)a;(void)b;(void)z; return false; }
void state_save(double a, double b, int z) { (void)a;(void)b;(void)z; }
void gps_net_init(GpsNet *g, const Config *c) { (void)g;(void)c; }
void gps_net_exit(GpsNet *g) { (void)g; }
bool gps_net_poll(GpsNet *g) { (void)g; return false; }
void navnet_init(const char *u) { (void)u; }
void navnet_exit(void) {}
bool navnet_search(const char *q, double a, double b) { (void)q;(void)a;(void)b; return true; }
bool navnet_route(double a, double b, double c, double d, bool w) { (void)a;(void)b;(void)c;(void)d;(void)w; return true; }
JobKind navnet_kind(void) { return JOB_SEARCH; }
const char *navnet_body(void) { return ""; }
const char *navnet_error(void) { return ""; }
void navnet_clear(void) {}
