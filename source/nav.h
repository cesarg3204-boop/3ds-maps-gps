#pragma once
/* Place search results and turn-by-turn routes (no libctru calls: unit-tested on PC). */
#include <stdbool.h>

#define MAX_PLACES 8

typedef struct {
	char   name[48];      /* ASCII, short */
	char   detail[120];   /* ASCII, rest of the address */
	double lat, lon;
} Place;

typedef struct {
	double lat, lon;      /* where the manoeuvre happens */
	double distance;      /* metres from this manoeuvre to the next one */
	int    geom_idx;      /* index into the route polyline */
	char   text[96];      /* e.g. "Turn left onto Calle Mayor" */
	char   arrow;         /* 'L' 'R' 'l' 'r' (slight) 'S' straight 'U' u-turn 'O' roundabout 'A' arrive 'D' depart */
} RouteStep;

typedef struct {
	int        npts;
	double    *lat, *lon;   /* polyline */
	double    *cum;         /* metres from the start to each point */
	int        nsteps;
	RouteStep *steps;
	double     distance;    /* metres */
	double     duration;    /* seconds */
} Route;

typedef struct {
	int    seg;             /* polyline segment we're on */
	double off_route_m;     /* distance from the route line */
	double along_m;         /* metres travelled along the route */
	int    next_step;       /* index of the next manoeuvre */
	double to_next_m;       /* metres to that manoeuvre */
	double remaining_m;
	double remaining_s;
	bool   arrived;
} NavProgress;

int  nav_parse_search(const char *json, Place *out, int max);
bool nav_parse_route(const char *json, Route *r, char *err, int errlen);
void route_free(Route *r);

/* Where am I on the route? Call with each new GPS position. */
void nav_progress(const Route *r, double lat, double lon, NavProgress *p);

double geo_distance_m(double lat1, double lon1, double lat2, double lon2);
void   format_distance(char *out, int n, double m);
void   utf8_to_ascii(char *out, int n, const char *in);
void   url_encode(char *out, int n, const char *in);
