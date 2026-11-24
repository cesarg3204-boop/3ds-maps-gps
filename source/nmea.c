#include "nmea.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_FIELDS 24

void nmea_init(GpsFix *fix)
{
	memset(fix, 0, sizeof(*fix));
	fix->course_deg = -1;
	fix->sats = -1;
	fix->hdop = -1;
	fix->hh = fix->mm = fix->ss = -1;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	c = (char)toupper((unsigned char)c);
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* "ddmm.mmmm" / "dddmm.mmmm" + hemisphere -> signed decimal degrees */
static bool parse_coord(const char *v, const char *hemi, double *out)
{
	if (!v[0] || !hemi[0]) return false;
	double raw = atof(v);
	int deg = (int)(raw / 100.0);
	double minutes = raw - deg * 100.0;
	if (minutes < 0 || minutes >= 60.0) return false;
	double d = deg + minutes / 60.0;
	if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
	else if (hemi[0] != 'N' && hemi[0] != 'E') return false;
	*out = d;
	return true;
}

static void parse_time(GpsFix *fix, const char *t)
{
	if (strlen(t) < 6) return;
	for (int i = 0; i < 6; i++) if (!isdigit((unsigned char)t[i])) return;
	fix->hh = (t[0]-'0')*10 + (t[1]-'0');
	fix->mm = (t[2]-'0')*10 + (t[3]-'0');
	fix->ss = (t[4]-'0')*10 + (t[5]-'0');
}

bool nmea_parse_sentence(GpsFix *fix, const char *line)
{
	char buf[256];
	const char *start = strchr(line, '$');
	if (!start) return false;
	size_t n = strcspn(start, "\r\n");
	if (n >= sizeof(buf) || n < 7) return false;
	memcpy(buf, start, n);
	buf[n] = 0;

	/* Checksum: XOR of everything between '$' and '*'. Optional, but if present it must match. */
	char *star = strchr(buf, '*');
	if (star) {
		if (hexval(star[1]) < 0 || hexval(star[2]) < 0) return false;
		unsigned want = (unsigned)(hexval(star[1]) * 16 + hexval(star[2]));
		unsigned sum = 0;
		for (char *p = buf + 1; p < star; p++) sum ^= (unsigned char)*p;
		if (sum != want) return false;
		*star = 0;
	}

	/* Split on commas, keeping empty fields. */
	char *f[MAX_FIELDS];
	int nf = 0;
	char *p = buf + 1;
	f[nf++] = p;
	while (*p && nf < MAX_FIELDS) {
		if (*p == ',') { *p = 0; f[nf++] = p + 1; }
		p++;
	}
	for (int i = nf; i < MAX_FIELDS; i++) f[i] = (char *)"";

	if (strlen(f[0]) != 5) return false;
	const char *type = f[0] + 2; /* skip talker id: GP, GN, GL, GA, BD... */

	if (strcmp(type, "RMC") == 0) {
		/* $xxRMC,time,status,lat,N,lon,E,speed_kn,course,date,... */
		parse_time(fix, f[1]);
		if (f[2][0] != 'A') { fix->valid = false; fix->count++; return true; }
		double lat, lon;
		if (!parse_coord(f[3], f[4], &lat) || !parse_coord(f[5], f[6], &lon)) return false;
		fix->lat = lat;
		fix->lon = lon;
		fix->speed_kmh = f[7][0] ? atof(f[7]) * 1.852 : 0;
		fix->course_deg = f[8][0] ? atof(f[8]) : -1;
		fix->valid = true;
		fix->count++;
		return true;
	}
	if (strcmp(type, "GGA") == 0) {
		/* $xxGGA,time,lat,N,lon,E,quality,sats,hdop,alt,M,... */
		parse_time(fix, f[1]);
		int q = atoi(f[6]);
		fix->sats = f[7][0] ? atoi(f[7]) : -1;
		fix->hdop = f[8][0] ? atof(f[8]) : -1;
		if (q <= 0) { fix->valid = false; fix->count++; return true; }
		double lat, lon;
		if (!parse_coord(f[2], f[3], &lat) || !parse_coord(f[4], f[5], &lon)) return false;
		fix->lat = lat;
		fix->lon = lon;
		fix->altitude_m = atof(f[9]);
		fix->valid = true;
		fix->count++;
		return true;
	}
	return false;
}

void nmea_stream_init(NmeaStream *s)
{
	s->len = 0;
}

int nmea_stream_feed(NmeaStream *s, GpsFix *fix, const char *data, int n)
{
	int updated = 0;
	for (int i = 0; i < n; i++) {
		char c = data[i];
		if (c == '\n' || c == '\r') {
			if (s->len > 0) {
				s->buf[s->len] = 0;
				if (nmea_parse_sentence(fix, s->buf)) updated++;
				s->len = 0;
			}
		} else if (c == '$') {
			s->len = 0;              /* resync on every sentence start */
			s->buf[s->len++] = c;
		} else if (s->len > 0 && s->len < (int)sizeof(s->buf) - 1) {
			s->buf[s->len++] = c;
		} else {
			s->len = 0;              /* overflow or junk: drop */
		}
	}
	return updated;
}
