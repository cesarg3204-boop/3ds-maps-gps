/*
 * Tile cache: decoded tiles live in a fixed pool of RAM slots. A background
 * worker thread loads requested tiles from the SD card and, when online,
 * downloads missing ones (saving them to the SD card for offline use).
 *
 * Pixel layout of a decoded tile: the 3DS top-screen framebuffer is stored
 * rotated (each screen column is contiguous, running bottom to top, 3 bytes
 * per pixel in B,G,R order). Tiles are stored the same way:
 *     data[(u * 256 + (255 - v)) * 3]   for tile pixel (u = column, v = row)
 * so a vertical run of tile pixels is a single memcpy into the framebuffer.
 */
#include "tiles.h"
#include "render.h"
#include <3ds.h>
#include <curl/curl.h>
#include <png.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define N_SLOTS    64
#define TILE_BYTES (TILE_SIZE * TILE_SIZE * 3)
#define RETRY_MS   4000
#define N_DL       2      /* OSM tile policy: at most 2 connections */

enum { S_EMPTY, S_QUEUED, S_LOADING, S_NEEDDL, S_DOWNLOADING, S_READY, S_MISSING };

typedef struct {
	int z, x, y;
	volatile int state;
	uint32_t last_used;
	uint64_t retry_at;   /* for S_MISSING: when to try again (0 = never) */
	uint8_t *data;
} Slot;

static Slot slots[N_SLOTS];
static LightLock lock;
static LightEvent wake;
static Thread loader, dlers[N_DL];
static LightEvent dl_wake;
static volatile bool quit;
static volatile bool online;
static volatile bool new_ready;
static volatile int downloaded;
static uint32_t frame_no = 1;
static char last_err[64];
static char tile_url[256], user_agent[160];

/* ---------- file helpers ---------- */

static void tile_path(char *out, size_t n, int z, int x, int y)
{
	snprintf(out, n, APP_DIR "/tiles/%d/%d/%d.png", z, x, y);
}

static void make_dirs(int z, int x)
{
	char p[128];
	snprintf(p, sizeof(p), APP_DIR "/tiles");           mkdir(p, 0777);
	snprintf(p, sizeof(p), APP_DIR "/tiles/%d", z);      mkdir(p, 0777);
	snprintf(p, sizeof(p), APP_DIR "/tiles/%d/%d", z, x); mkdir(p, 0777);
}

typedef struct { uint8_t *p; size_t len, cap; } Buf;

static bool read_file(const char *path, Buf *b)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return false; }
	b->p = malloc(n);
	b->len = b->p ? fread(b->p, 1, n, f) : 0;
	fclose(f);
	return b->len == (size_t)n;
}

static bool decode_png(const uint8_t *src, size_t len, uint8_t *out, uint8_t *tmp)
{
	png_image img;
	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&img, src, len)) return false;
	if (img.width != TILE_SIZE || img.height != TILE_SIZE) { png_image_free(&img); return false; }
	img.format = PNG_FORMAT_BGR;
	png_color bg = { 0xF2, 0xEF, 0xE9 };  /* OSM land colour behind transparency */
	if (!png_image_finish_read(&img, &bg, tmp, 0, NULL)) return false;

	tile_transpose(tmp, out);
	return true;
}

/* ---------- download ---------- */

static size_t curl_write(void *ptr, size_t sz, size_t nm, void *ud)
{
	Buf *b = ud;
	size_t n = sz * nm;
	if (b->len + n > 4 * 1024 * 1024) return 0;
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 32768;
		while (cap < b->len + n) cap *= 2;
		uint8_t *p = realloc(b->p, cap);
		if (!p) return 0;
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->len, ptr, n);
	b->len += n;
	return n;
}

static void build_url(char *out, size_t n, int z, int x, int y)
{
	char tmp[16];
	size_t o = 0;
	for (const char *s = tile_url; *s && o + 1 < n; ) {
		const char *rep = NULL;
		if (!strncmp(s, "{z}", 3)) { snprintf(tmp, sizeof(tmp), "%d", z); rep = tmp; }
		else if (!strncmp(s, "{x}", 3)) { snprintf(tmp, sizeof(tmp), "%d", x); rep = tmp; }
		else if (!strncmp(s, "{y}", 3)) { snprintf(tmp, sizeof(tmp), "%d", y); rep = tmp; }
		if (rep) {
			for (; *rep && o + 1 < n; rep++) out[o++] = *rep;
			s += 3;
		} else out[o++] = *s++;
	}
	out[o] = 0;
}

/* 1 = ok, 0 = permanent failure (404 etc.), -1 = temporary failure */
static int download(CURL *c, int z, int x, int y, Buf *b)
{
	char url[320];
	build_url(url, sizeof(url), z, x, y);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
	CURLcode rc = curl_easy_perform(c);
	if (rc != CURLE_OK) {
		snprintf(last_err, sizeof(last_err), "net: %.50s", curl_easy_strerror(rc));
		return -1;
	}
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	if (code != 200) {
		snprintf(last_err, sizeof(last_err), "HTTP %ld for %d/%d/%d", code, z, x, y);
		return (code == 404 || code == 403) ? 0 : -1;
	}
	return 1;
}

/* ---------- workers ----------
 * One loader thread reads tiles from the SD card (fast). Tiles that aren't
 * there are handed to N_DL downloader threads, so a slow download never
 * holds up tiles that are already cached. */

static Slot *pick(int from_state, int to_state, int *z, int *x, int *y)
{
	LightLock_Lock(&lock);
	Slot *s = NULL;
	for (int i = 0; i < N_SLOTS; i++)   /* most recently wanted first */
		if (slots[i].state == from_state && (!s || slots[i].last_used > s->last_used))
			s = &slots[i];
	if (s) { s->state = to_state; *z = s->z; *x = s->x; *y = s->y; }
	LightLock_Unlock(&lock);
	return s;
}

static void finish(Slot *s, bool ok, uint64_t retry_at)
{
	LightLock_Lock(&lock);
	s->state = ok ? S_READY : S_MISSING;
	s->retry_at = retry_at;
	if (ok) new_ready = true;
	LightLock_Unlock(&lock);
}

static void loader_main(void *arg)
{
	(void)arg;
	uint8_t *tmp = malloc(TILE_BYTES);
	while (!quit) {
		int z, x, y;
		Slot *s = pick(S_QUEUED, S_LOADING, &z, &x, &y);
		if (!s) { LightEvent_Wait(&wake); continue; }

		char path[128];
		tile_path(path, sizeof(path), z, x, y);
		Buf b = {0};
		bool ok = false;
		if (read_file(path, &b)) {
			ok = decode_png(b.p, b.len, s->data, tmp);
			if (!ok) remove(path);          /* corrupt cache file: refetch */
		}
		free(b.p);
		if (ok) finish(s, true, 0);
		else if (online) {
			LightLock_Lock(&lock);
			s->state = S_NEEDDL;
			LightLock_Unlock(&lock);
			LightEvent_Signal(&dl_wake);
		} else finish(s, false, 0);
	}
	free(tmp);
}

static void downloader_main(void *arg)
{
	(void)arg;
	uint8_t *tmp = malloc(TILE_BYTES);
	CURL *c = curl_easy_init();
	if (c) {
		curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);   /* reuse the TLS connection */
	}
	while (!quit) {
		int z, x, y;
		Slot *s = c && online ? pick(S_NEEDDL, S_DOWNLOADING, &z, &x, &y) : NULL;
		if (!s) { LightEvent_WaitTimeout(&dl_wake, 200000000LL); continue; }

		Buf b = {0};
		bool ok = false;
		uint64_t retry_at = 0;
		int r = download(c, z, x, y, &b);
		if (r == 1 && decode_png(b.p, b.len, s->data, tmp)) {
			ok = true;
			char path[128], part[140];
			tile_path(path, sizeof(path), z, x, y);
			make_dirs(z, x);
			snprintf(part, sizeof(part), "%s.part", path);
			FILE *f = fopen(part, "wb");
			if (f) {
				bool w = fwrite(b.p, 1, b.len, f) == b.len;
				fclose(f);
				if (w) rename(part, path); else remove(part);
			}
			downloaded++;
		} else if (r == 1) {
			snprintf(last_err, sizeof(last_err), "not a 256px PNG: %d/%d/%d", z, x, y);
		} else if (r == -1) {
			retry_at = osGetTime() + RETRY_MS;
		}
		free(b.p);
		finish(s, ok, retry_at);
	}
	if (c) curl_easy_cleanup(c);
	free(tmp);
}

/* ---------- public ---------- */

void tiles_init(const Config *cfg)
{
	snprintf(tile_url, sizeof(tile_url), "%s", cfg->tile_url);
	snprintf(user_agent, sizeof(user_agent), "%s", cfg->user_agent);
	online = cfg->online;
	LightLock_Init(&lock);
	LightEvent_Init(&wake, RESET_ONESHOT);
	LightEvent_Init(&dl_wake, RESET_ONESHOT);
	for (int i = 0; i < N_SLOTS; i++) {
		slots[i].state = S_EMPTY;
		slots[i].data = malloc(TILE_BYTES);
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	quit = false;
	loader = threadCreate(loader_main, NULL, 32 * 1024, prio + 1, -2, false);
	for (int i = 0; i < N_DL; i++)
		dlers[i] = threadCreate(downloader_main, NULL, 64 * 1024, prio + 1, -2, false);
}

void tiles_exit(void)
{
	quit = true;
	LightEvent_Signal(&wake);
	LightEvent_Signal(&dl_wake);
	if (loader) { threadJoin(loader, U64_MAX); threadFree(loader); }
	for (int i = 0; i < N_DL; i++)
		if (dlers[i]) { threadJoin(dlers[i], U64_MAX); threadFree(dlers[i]); }
	curl_global_cleanup();
	for (int i = 0; i < N_SLOTS; i++) free(slots[i].data);
}

bool tiles_begin_frame(void)
{
	frame_no++;
	bool r = new_ready;
	new_ready = false;
	return r;
}

/* stamp = frame_no for tiles on screen, frame_no - 1 for prefetch (lower priority,
 * and a prefetch never pushes out a tile that is on screen or recently prefetched) */
static const uint8_t *request(int z, int x, int y, uint32_t stamp, bool *missing)
{
	*missing = false;
	const uint8_t *ret = NULL;
	bool signal = false;

	LightLock_Lock(&lock);
	Slot *hit = NULL;
	for (int i = 0; i < N_SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state != S_EMPTY && s->z == z && s->x == x && s->y == y) { hit = s; break; }
	}
	if (hit) {
		if (hit->last_used < stamp) hit->last_used = stamp;
		if (hit->state == S_READY) ret = hit->data;
		else if (hit->state == S_MISSING) {
			if (hit->retry_at && online && osGetTime() >= hit->retry_at) {
				hit->state = S_QUEUED;
				signal = true;
			} else *missing = true;
		}
	} else {
		/* evict: empty slot first, else the least recently used one */
		Slot *v = NULL;
		for (int i = 0; i < N_SLOTS; i++) {
			Slot *s = &slots[i];
			if (s->state == S_LOADING || s->state == S_DOWNLOADING || s->last_used >= stamp) continue;
			if (s->state == S_EMPTY) { v = s; break; }
			if (!v || s->last_used < v->last_used) v = s;
		}
		if (v) {
			v->z = z; v->x = x; v->y = y;
			v->state = S_QUEUED;
			v->last_used = stamp;
			v->retry_at = 0;
			signal = true;
		}
	}
	LightLock_Unlock(&lock);
	if (signal) LightEvent_Signal(&wake);
	return ret;
}

const uint8_t *tiles_get(int z, int x, int y, bool *missing)
{
	return request(z, x, y, frame_no, missing);
}

void tiles_prefetch(int z, int x, int y)
{
	bool m;
	int n = 1 << z;
	if (y < 0 || y >= n) return;
	x = ((x % n) + n) % n;
	request(z, x, y, frame_no - 1, &m);
}

const uint8_t *tiles_peek(int z, int x, int y)
{
	const uint8_t *ret = NULL;
	LightLock_Lock(&lock);
	for (int i = 0; i < N_SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state == S_READY && s->z == z && s->x == x && s->y == y) {
			s->last_used = frame_no;    /* keep it while it's drawn this frame */
			ret = s->data;
			break;
		}
	}
	LightLock_Unlock(&lock);
	return ret;
}

void tiles_set_online(bool on)
{
	LightLock_Lock(&lock);
	online = on;
	for (int i = 0; i < N_SLOTS; i++) {
		if (on && slots[i].state == S_MISSING) slots[i].state = S_EMPTY;      /* try again */
		if (!on && slots[i].state == S_NEEDDL) slots[i].state = S_MISSING;    /* won't download */
	}
	LightLock_Unlock(&lock);
}

bool tiles_online(void) { return online; }

int tiles_pending(void)
{
	int n = 0;
	for (int i = 0; i < N_SLOTS; i++)
		if (slots[i].state == S_QUEUED || slots[i].state == S_LOADING ||
		    slots[i].state == S_NEEDDL || slots[i].state == S_DOWNLOADING) n++;
	return n;
}

int tiles_downloaded(void) { return downloaded; }
const char *tiles_last_error(void) { return last_err; }
