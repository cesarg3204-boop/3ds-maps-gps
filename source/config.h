#pragma once
#include <stdbool.h>

#define APP_DIR "sdmc:/3ds/3dsmaps"

typedef struct {
	int    web_port;         /* HTTP page for the phone's browser (0 = off) */
	int    udp_port;         /* listen for NMEA datagrams (phone pushes to 3DS) */
	char   phone_ip[64];     /* "auto" = hotspot gateway, "" / "off" = no TCP */
	int    tcp_port;         /* connect to phone's NMEA TCP server (e.g. GPS2IP) */
	bool   online;           /* download missing tiles over Wi-Fi */
	char   tile_url[256];    /* {z} {x} {y} placeholders */
	char   user_agent[160];
	int    zoom;
	double start_lat, start_lon;
} Config;

void config_defaults(Config *c);
void config_load(Config *c, const char *path);   /* creates the file with defaults if missing */
void config_save(const Config *c, const char *path);

/* Last viewed position, so the map opens where you left it. */
bool state_load(double *lat, double *lon, int *zoom);
void state_save(double lat, double lon, int zoom);
