#include "nav.h"
#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- text helpers ---------- */

/* Latin-1 range (U+00C0..U+00FF) to plain ASCII, so names show in the 3DS console font. */
static const char *latin1_ascii[64] = {
	"A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
	"D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
	"a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
	"d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y"
};

void utf8_to_ascii(char *out, int n, const char *in)
{
	int o = 0;
	const unsigned char *s = (const unsigned char *)in;
	while (*s && o < n - 1) {
		unsigned cp;
		int len;
		if (*s < 0x80) { cp = *s; len = 1; }
		else if ((*s & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); len = 2; }
		else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) { cp = 0x2014; len = 3; if (s[0] == 0xE2 && s[1] == 0x80 && (s[2] == 0x98 || s[2] == 0x99)) cp = '\''; }
		else if ((*s & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { cp = '?'; len = 4; }
		else { cp = '?'; len = 1; }
		const char *rep = NULL;
		char one[2] = {0, 0};
		if (cp < 0x80) { one[0] = (char)cp; rep = one; }
		else if (cp >= 0xC0 && cp <= 0xFF) rep = latin1_ascii[cp - 0xC0];
		else if (cp == 0xA1 || cp == 0xBF) rep = "";
		else if (cp == 0x2014) rep = "-";
		else rep = "?";
		for (; *rep && o < n - 1; rep++) out[o++] = *rep;
		for (int i = 0; i < len && *s; i++) s++;
	}
	out[o] = 0;
}

void url_encode(char *out, int n, const char *in)
{
	static const char hex[] = "0123456789ABCDEF";
	int o = 0;
	for (const unsigned char *s = (const unsigned char *)in; *s && o < n - 4; s++) {
		if (isalnum(*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') out[o++] = *s;
		else if (*s == ' ') out[o++] = '+';
		else { out[o++] = '%'; out[o++] = hex[*s >> 4]; out[o++] = hex[*s & 15]; }
	}
	out[o] = 0;
}

double geo_distance_m(double lat1, double lon1, double lat2, double lon2)
{
	/* equirectangular approximation: plenty for street-scale distances */
	double k = M_PI / 180.0;
	double x = (lon2 - lon1) * k * cos((lat1 + lat2) * 0.5 * k);
	double y = (lat2 - lat1) * k;
	return sqrt(x * x + y * y) * 6371000.0;
}

void format_distance(char *out, int n, double m)
{
	if (m < 50) snprintf(out, n, "%d m", (int)(m + 0.5));
	else if (m < 1000) snprintf(out, n, "%d m", ((int)(m + 5) / 10) * 10);
	else if (m < 10000) snprintf(out, n, "%.1f km", m / 1000.0);
	else snprintf(out, n, "%d km", (int)(m / 1000.0 + 0.5));
}

/* ---------- search (Nominatim jsonv2) ---------- */

int nav_parse_search(const char *json, Place *out, int max)
{
	json_error_t err;
	json_t *root = json_loads(json, 0, &err);
	if (!root || !json_is_array(root)) { json_decref(root); return -1; }
	int n = 0;
	size_t i;
	json_t *it;
	json_array_foreach(root, i, it) {
		if (n >= max) break;
		const char *lat = json_string_value(json_object_get(it, "lat"));
		const char *lon = json_string_value(json_object_get(it, "lon"));
		const char *disp = json_string_value(json_object_get(it, "display_name"));
		const char *name = json_string_value(json_object_get(it, "name"));
		if (!lat || !lon || !disp) continue;
		Place *p = &out[n++];
		p->lat = atof(lat);
		p->lon = atof(lon);
		/* name = OSM name, or the first part of the address; detail = the rest */
		const char *comma = strchr(disp, ',');
		char first[200];
		int fl = comma ? (int)(comma - disp) : (int)strlen(disp);
		if (fl > 199) fl = 199;
		memcpy(first, disp, fl);
		first[fl] = 0;
		utf8_to_ascii(p->name, sizeof(p->name), (name && *name) ? name : first);
		const char *rest = comma ? comma + 1 : "";
		while (*rest == ' ') rest++;
		/* if the name was something else (e.g. house number first), keep the whole address */
		utf8_to_ascii(p->detail, sizeof(p->detail), (name && *name && strcmp(name, first) != 0) ? disp : rest);
	}
	json_decref(root);
	return n;
}

/* ---------- route (OSRM v5) ---------- */

static int decode_polyline(const char *s, double **lat, double **lon)
{
	int cap = 64, n = 0;
	*lat = malloc(cap * sizeof(double));
	*lon = malloc(cap * sizeof(double));
	long la = 0, lo = 0;
	while (*s) {
		long v[2];
		for (int k = 0; k < 2; k++) {
			long result = 0;
			int shift = 0, b;
			do {
				if (!*s) goto done;
				b = *s++ - 63;
				result |= (long)(b & 0x1f) << shift;
				shift += 5;
			} while (b >= 0x20);
			v[k] = (result & 1) ? ~(result >> 1) : (result >> 1);
		}
		la += v[0];
		lo += v[1];
		if (n == cap) {
			cap *= 2;
			*lat = realloc(*lat, cap * sizeof(double));
			*lon = realloc(*lon, cap * sizeof(double));
		}
		(*lat)[n] = la / 1e5;
		(*lon)[n] = lo / 1e5;
		n++;
	}
done:
	return n;
}

static const char *ordinal(int n)
{
	static char buf[16];
	const char *suf = (n % 100 >= 11 && n % 100 <= 13) ? "th" :
	                  n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
	snprintf(buf, sizeof(buf), "%d%s", n, suf);
	return buf;
}

static const char *compass(double bearing)
{
	static const char *d[8] = { "north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest" };
	int i = (int)floor(fmod(bearing + 22.5 + 360.0, 360.0) / 45.0);
	return d[i & 7];
}

static void make_instruction(RouteStep *st, const char *type, const char *mod, const char *name,
                             int exit, double bearing_after, bool last)
{
	char road[64];
	utf8_to_ascii(road, sizeof(road), name ? name : "");
	const char *m = mod ? mod : "";
	char dir[24];

	st->arrow = 'S';
	if (!strcmp(m, "left")) { st->arrow = 'L'; strcpy(dir, "left"); }
	else if (!strcmp(m, "right")) { st->arrow = 'R'; strcpy(dir, "right"); }
	else if (!strcmp(m, "sharp left")) { st->arrow = 'L'; strcpy(dir, "sharp left"); }
	else if (!strcmp(m, "sharp right")) { st->arrow = 'R'; strcpy(dir, "sharp right"); }
	else if (!strcmp(m, "slight left")) { st->arrow = 'l'; strcpy(dir, "slight left"); }
	else if (!strcmp(m, "slight right")) { st->arrow = 'r'; strcpy(dir, "slight right"); }
	else if (!strcmp(m, "uturn")) { st->arrow = 'U'; strcpy(dir, "U-turn"); }
	else strcpy(dir, "straight");

	char onto[80] = "";
	if (road[0]) snprintf(onto, sizeof(onto), " onto %s", road);

	if (!strcmp(type, "depart")) {
		st->arrow = 'D';
		snprintf(st->text, sizeof(st->text), "Head %s%s%s", compass(bearing_after),
		         road[0] ? " on " : "", road);
	} else if (!strcmp(type, "arrive") || last) {
		st->arrow = 'A';
		snprintf(st->text, sizeof(st->text), "Arrive at your destination");
	} else if (!strcmp(type, "roundabout") || !strcmp(type, "rotary")) {
		st->arrow = 'O';
		if (exit > 0) snprintf(st->text, sizeof(st->text), "At the roundabout, take the %s exit%s", ordinal(exit), onto);
		else snprintf(st->text, sizeof(st->text), "Enter the roundabout%s", onto);
	} else if (!strcmp(type, "exit roundabout") || !strcmp(type, "exit rotary")) {
		snprintf(st->text, sizeof(st->text), "Exit the roundabout%s", onto);
	} else if (!strcmp(m, "uturn")) {
		snprintf(st->text, sizeof(st->text), "Make a U-turn%s", onto);
	} else if (!strcmp(type, "merge")) {
		snprintf(st->text, sizeof(st->text), "Merge%s%s", strcmp(dir, "straight") ? " " : "", strcmp(dir, "straight") ? dir : "");
		if (road[0]) { strncat(st->text, onto, sizeof(st->text) - strlen(st->text) - 1); }
	} else if (!strcmp(type, "on ramp")) {
		snprintf(st->text, sizeof(st->text), "Take the ramp%s%s%s", strcmp(dir, "straight") ? " on the " : "",
		         strcmp(dir, "straight") ? dir : "", onto);
	} else if (!strcmp(type, "off ramp")) {
		snprintf(st->text, sizeof(st->text), "Take the exit%s%s%s", strcmp(dir, "straight") ? " on the " : "",
		         strcmp(dir, "straight") ? dir : "", onto);
	} else if (!strcmp(type, "fork")) {
		snprintf(st->text, sizeof(st->text), "Keep %s at the fork%s",
		         strstr(dir, "left") ? "left" : strstr(dir, "right") ? "right" : "straight", onto);
	} else if (!strcmp(type, "new name") || !strcmp(type, "continue") || !strcmp(dir, "straight")) {
		if (!strcmp(dir, "straight") || !strcmp(type, "new name"))
			snprintf(st->text, sizeof(st->text), "Continue%s", road[0] ? onto : " straight");
		else
			snprintf(st->text, sizeof(st->text), "Keep %s%s", strstr(dir, "left") ? "left" : "right", onto);
	} else {
		/* turn, end of road, notification, ... */
		snprintf(st->text, sizeof(st->text), "Turn %s%s", dir, onto);
	}
}

void route_free(Route *r)
{
	free(r->lat); free(r->lon); free(r->cum); free(r->steps);
	memset(r, 0, sizeof(*r));
}

bool nav_parse_route(const char *json, Route *r, char *err, int errlen)
{
	memset(r, 0, sizeof(*r));
	json_error_t jerr;
	json_t *root = json_loads(json, 0, &jerr);
	if (!root) { snprintf(err, errlen, "bad reply from route server"); return false; }
	const char *code = json_string_value(json_object_get(root, "code"));
	json_t *route = json_array_get(json_object_get(root, "routes"), 0);
	if (!code || strcmp(code, "Ok") || !route) {
		const char *msg = json_string_value(json_object_get(root, "message"));
		snprintf(err, errlen, "%s", code && !strcmp(code, "NoRoute") ? "no route found" :
		                             msg ? msg : "no route found");
		json_decref(root);
		return false;
	}
	const char *geom = json_string_value(json_object_get(route, "geometry"));
	r->distance = json_number_value(json_object_get(route, "distance"));
	r->duration = json_number_value(json_object_get(route, "duration"));
	r->npts = geom ? decode_polyline(geom, &r->lat, &r->lon) : 0;
	if (r->npts < 2) { snprintf(err, errlen, "route has no shape"); json_decref(root); route_free(r); return false; }

	r->cum = malloc(r->npts * sizeof(double));
	r->cum[0] = 0;
	for (int i = 1; i < r->npts; i++)
		r->cum[i] = r->cum[i - 1] + geo_distance_m(r->lat[i - 1], r->lon[i - 1], r->lat[i], r->lon[i]);

	/* steps from every leg */
	int total = 0;
	size_t li, si;
	json_t *leg, *step;
	json_array_foreach(json_object_get(route, "legs"), li, leg)
		total += json_array_size(json_object_get(leg, "steps"));
	r->steps = calloc(total ? total : 1, sizeof(RouteStep));
	int search_from = 0;
	json_array_foreach(json_object_get(route, "legs"), li, leg) {
		json_t *steps = json_object_get(leg, "steps");
		json_array_foreach(steps, si, step) {
			json_t *man = json_object_get(step, "maneuver");
			json_t *loc = json_object_get(man, "location");
			RouteStep *st = &r->steps[r->nsteps];
			st->lon = json_number_value(json_array_get(loc, 0));
			st->lat = json_number_value(json_array_get(loc, 1));
			st->distance = json_number_value(json_object_get(step, "distance"));
			const char *type = json_string_value(json_object_get(man, "type"));
			const char *mod = json_string_value(json_object_get(man, "modifier"));
			const char *name = json_string_value(json_object_get(step, "name"));
			const char *ref = json_string_value(json_object_get(step, "ref"));
			if ((!name || !*name) && ref) name = ref;
			int exit = (int)json_integer_value(json_object_get(man, "exit"));
			double ba = json_number_value(json_object_get(man, "bearing_after"));
			bool last = (li == json_array_size(json_object_get(route, "legs")) - 1) && si == json_array_size(steps) - 1;
			make_instruction(st, type ? type : "turn", mod, name, exit, ba, last);

			/* match the manoeuvre to the nearest polyline vertex, moving forward only */
			int best = search_from;
			double bd = 1e18;
			for (int i = search_from; i < r->npts; i++) {
				double d = geo_distance_m(st->lat, st->lon, r->lat[i], r->lon[i]);
				if (d < bd) { bd = d; best = i; }
				if (d < 0.5) break;
			}
			st->geom_idx = best;
			search_from = best;
			r->nsteps++;
		}
	}
	json_decref(root);
	if (r->nsteps == 0) { snprintf(err, errlen, "route has no directions"); route_free(r); return false; }
	return true;
}

/* ---------- progress along the route ---------- */

void nav_progress(const Route *r, double lat, double lon, NavProgress *p)
{
	/* local flat projection around the current position, in metres */
	double k = M_PI / 180.0 * 6371000.0, ck = cos(lat * M_PI / 180.0);
	double best = 1e18, best_t = 0, fwd = 1e18, fwd_t = 0;
	int best_i = 0, fwd_i = -1, prev = p->seg;
	for (int i = 0; i + 1 < r->npts; i++) {
		double ax = (r->lon[i] - lon) * k * ck, ay = (r->lat[i] - lat) * k;
		double bx = (r->lon[i + 1] - lon) * k * ck, by = (r->lat[i + 1] - lat) * k;
		double dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy;
		double t = L2 > 0 ? -(ax * dx + ay * dy) / L2 : 0;
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		double px = ax + t * dx, py = ay + t * dy;
		double d = sqrt(px * px + py * py);
		if (d < best) { best = d; best_i = i; best_t = t; }
		if (i >= prev && d < fwd) { fwd = d; fwd_i = i; fwd_t = t; }
	}
	/* where the route doubles back on itself, stay on the part ahead of us */
	if (fwd_i >= 0 && fwd < best + 15.0) { best = fwd; best_i = fwd_i; best_t = fwd_t; }
	p->seg = best_i;
	p->off_route_m = best;
	double seglen = r->cum[best_i + 1] - r->cum[best_i];
	p->along_m = r->cum[best_i] + best_t * seglen;
	double total = r->cum[r->npts - 1];
	p->remaining_m = total - p->along_m;
	if (p->remaining_m < 0) p->remaining_m = 0;
	p->remaining_s = total > 0 ? r->duration * p->remaining_m / total : 0;

	/* next manoeuvre = first step whose point is ahead of us (skip the departure) */
	p->next_step = r->nsteps - 1;
	for (int s = 1; s < r->nsteps; s++) {
		if (r->cum[r->steps[s].geom_idx] > p->along_m + 3.0) { p->next_step = s; break; }
	}
	p->to_next_m = r->cum[r->steps[p->next_step].geom_idx] - p->along_m;
	if (p->to_next_m < 0) p->to_next_m = 0;
	p->arrived = p->remaining_m < 20.0 && p->off_route_m < 40.0;
}
