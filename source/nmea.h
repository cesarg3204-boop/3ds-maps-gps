#pragma once
#include <stdbool.h>

/* Position state built up from NMEA 0183 sentences ($xxRMC and $xxGGA). */
typedef struct {
	bool   valid;        /* RMC status 'A' (or GGA fix quality > 0) */
	double lat, lon;     /* degrees, south/west negative */
	double speed_kmh;    /* from RMC */
	double course_deg;   /* from RMC, -1 if unknown */
	double altitude_m;   /* from GGA */
	int    sats;         /* from GGA, -1 if unknown */
	double hdop;         /* from GGA, -1 if unknown */
	int    hh, mm, ss;   /* UTC time of last fix, -1 if unknown */
	unsigned long count; /* number of accepted sentences */
} GpsFix;

void nmea_init(GpsFix *fix);

/* Feed one complete sentence (with or without trailing CR/LF).
 * Returns true if it was a recognised, checksum-valid sentence that updated *fix. */
bool nmea_parse_sentence(GpsFix *fix, const char *line);

/* Streaming helper: feed arbitrary bytes, sentences are split on newlines.
 * Returns the number of sentences that updated *fix. */
typedef struct {
	char buf[256];
	int  len;
} NmeaStream;

void nmea_stream_init(NmeaStream *s);
int  nmea_stream_feed(NmeaStream *s, GpsFix *fix, const char *data, int n);
