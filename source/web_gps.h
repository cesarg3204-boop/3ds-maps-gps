#pragma once
#include <stdbool.h>
#include "nmea.h"

/* Parse the body the phone page POSTs to /pos:
 *   "lat,lon,accuracy_m,speed_mps,heading_deg,altitude_m"
 * (empty fields allowed for speed/heading/altitude). Updates *fix. */
bool web_gps_parse(GpsFix *fix, const char *body);

/* The page served at "/" (HTML + JS). */
extern const char WEB_GPS_PAGE[];
