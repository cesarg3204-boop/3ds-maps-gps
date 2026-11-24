#pragma once
/* Background HTTP requests for place search (Nominatim) and routing (OSRM). */
#include <stdbool.h>

typedef enum { JOB_IDLE, JOB_RUNNING, JOB_DONE, JOB_FAILED } JobState;
typedef enum { JOB_SEARCH, JOB_ROUTE } JobKind;

void navnet_init(const char *user_agent);
void navnet_exit(void);

/* Start a request; returns false if one is already running. */
bool navnet_search(const char *query, double near_lat, double near_lon);
bool navnet_route(double from_lat, double from_lon, double to_lat, double to_lon, bool walking);

JobState    navnet_state(void);
JobKind     navnet_kind(void);
const char *navnet_body(void);   /* response text when JOB_DONE */
const char *navnet_error(void);  /* message when JOB_FAILED */
void        navnet_clear(void);  /* back to JOB_IDLE, frees the body */
