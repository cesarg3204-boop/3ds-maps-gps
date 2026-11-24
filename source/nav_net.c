#include "nav_net.h"
#include "nav.h"
#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public OpenStreetMap services. Nominatim: max 1 request/second, no autocomplete
 * (we only search when you press OK). Routing: FOSSGIS OSRM servers for OSM data. */
#define SEARCH_URL "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=8"
#define ROUTE_CAR  "https://routing.openstreetmap.de/routed-car/route/v1/driving/"
#define ROUTE_FOOT "https://routing.openstreetmap.de/routed-foot/route/v1/driving/"

static Thread thread;
static LightEvent wake;
static volatile bool quit;
static volatile JobState state = JOB_IDLE;
static JobKind kind;
static char url[1024];
static char ua[160];
static char err[96];
static char *body;
static size_t body_len, body_cap;
static u64 last_search_ms;
static bool route_plain_http;

static size_t on_data(void *ptr, size_t sz, size_t nm, void *ud)
{
	(void)ud;
	size_t n = sz * nm;
	if (body_len + n + 1 > 8 * 1024 * 1024) return 0;
	if (body_len + n + 1 > body_cap) {
		size_t cap = body_cap ? body_cap * 2 : 65536;
		while (cap < body_len + n + 1) cap *= 2;
		char *p = realloc(body, cap);
		if (!p) return 0;
		body = p;
		body_cap = cap;
	}
	memcpy(body + body_len, ptr, n);
	body_len += n;
	body[body_len] = 0;
	return n;
}

static void worker(void *arg)
{
	(void)arg;
	while (!quit) {
		LightEvent_Wait(&wake);
		if (quit) break;
		if (state != JOB_RUNNING) continue;

		/* Nominatim allows one request per second */
		if (kind == JOB_SEARCH) {
			u64 now = osGetTime();
			if (now - last_search_ms < 1100) svcSleepThread((1100 - (now - last_search_ms)) * 1000000ULL);
			last_search_ms = osGetTime();
		}

		/* The routing server's HTTPS doesn't handshake with the 3DS's TLS library
		 * (seen as "SSL connect error"), so routes fall back to plain HTTP once
		 * that happens. Route requests only contain the two end points. */
		if (kind == JOB_ROUTE && route_plain_http && !strncmp(url, "https://", 8))
			memmove(url + 4, url + 5, strlen(url + 5) + 1);   /* https -> http */

		CURLcode rc = CURLE_OK;
		long code = 0;
		for (int attempt = 0; attempt < 2; attempt++) {
			body_len = 0;
			if (body) body[0] = 0;
			CURL *c = curl_easy_init();
			if (!c) { rc = CURLE_OUT_OF_MEMORY; break; }
			curl_easy_setopt(c, CURLOPT_URL, url);
			curl_easy_setopt(c, CURLOPT_USERAGENT, ua);
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
			curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
			curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L);
			curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
			curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
			rc = curl_easy_perform(c);
			code = 0;
			curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
			curl_easy_cleanup(c);
			if (rc == CURLE_SSL_CONNECT_ERROR && kind == JOB_ROUTE && !strncmp(url, "https://", 8)) {
				route_plain_http = true;
				memmove(url + 4, url + 5, strlen(url + 5) + 1);
				continue;
			}
			break;
		}

		if (rc != CURLE_OK) {
			snprintf(err, sizeof(err), "Connection failed: %.60s", curl_easy_strerror(rc));
			state = JOB_FAILED;
		} else if (code != 200 && !(kind == JOB_ROUTE && code == 400 && body_len)) {
			/* OSRM answers 400 with a JSON "NoRoute"/"InvalidQuery" body: let the parser report it */
			snprintf(err, sizeof(err), "Server error (HTTP %ld). Try again later.", code);
			state = JOB_FAILED;
		} else if (!body_len) {
			snprintf(err, sizeof(err), "Empty reply from server");
			state = JOB_FAILED;
		} else {
			state = JOB_DONE;
		}
	}
}

void navnet_init(const char *user_agent)
{
	snprintf(ua, sizeof(ua), "%s", user_agent);
	LightEvent_Init(&wake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	quit = false;
	thread = threadCreate(worker, NULL, 64 * 1024, prio + 1, -2, false);
}

void navnet_exit(void)
{
	quit = true;
	LightEvent_Signal(&wake);
	if (thread) { threadJoin(thread, U64_MAX); threadFree(thread); }
	free(body);
	body = NULL;
	body_cap = body_len = 0;
}

static bool start(JobKind k)
{
	kind = k;
	err[0] = 0;
	state = JOB_RUNNING;
	LightEvent_Signal(&wake);
	return true;
}

bool navnet_search(const char *query, double near_lat, double near_lon)
{
	if (state == JOB_RUNNING) return false;
	char q[400];
	url_encode(q, sizeof(q), query);
	/* viewbox biases results towards where you are (without excluding others) */
	snprintf(url, sizeof(url), SEARCH_URL "&q=%s&viewbox=%.4f,%.4f,%.4f,%.4f",
	         q, near_lon - 0.5, near_lat + 0.5, near_lon + 0.5, near_lat - 0.5);
	return start(JOB_SEARCH);
}

bool navnet_route(double from_lat, double from_lon, double to_lat, double to_lon, bool walking)
{
	if (state == JOB_RUNNING) return false;
	snprintf(url, sizeof(url), "%s%.6f,%.6f;%.6f,%.6f?overview=full&steps=true&geometries=polyline",
	         walking ? ROUTE_FOOT : ROUTE_CAR, from_lon, from_lat, to_lon, to_lat);
	return start(JOB_ROUTE);
}

JobState navnet_state(void) { return state; }
JobKind navnet_kind(void) { return kind; }
const char *navnet_body(void) { return body ? body : ""; }
const char *navnet_error(void) { return err; }

void navnet_clear(void)
{
	if (state == JOB_RUNNING) return;
	state = JOB_IDLE;
	body_len = 0;
	if (body) body[0] = 0;
}
