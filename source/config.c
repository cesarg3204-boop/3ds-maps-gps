#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#define STATE_PATH APP_DIR "/last_position.txt"

void config_defaults(Config *c)
{
	memset(c, 0, sizeof(*c));
	c->web_port = 8080;
	c->udp_port = 10110;                 /* IANA port for NMEA-0183 over IP */
	strcpy(c->phone_ip, "auto");
	c->tcp_port = 11123;                 /* GPS2IP default */
	c->online = true;
	strcpy(c->tile_url, "https://tile.openstreetmap.org/{z}/{x}/{y}.png");
	strcpy(c->user_agent, "3DSMaps/1.0 (Nintendo 3DS homebrew map viewer)");
	c->zoom = 15;
	c->start_lat = 40.4168;              /* Madrid; only used before the first fix */
	c->start_lon = -3.7038;
}

void config_save(const Config *c, const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f,
		"# 3DS Maps configuration. Lines starting with # are comments.\n"
		"\n"
		"# Web page for the phone: open http://<3DS IP>:<web_port> in Chrome. 0 = off.\n"
		"web_port=%d\n"
		"\n"
		"# UDP port the 3DS listens on for NMEA sentences pushed by the phone.\n"
		"udp_port=%d\n"
		"\n"
		"# Phone IP for TCP mode (the 3DS connects to an NMEA server on the phone,\n"
		"# e.g. Share GPS or GPS2IP). 'auto' = find it on the hotspot, 'off' = disabled.\n"
		"# You can also change it in the app: Info > Phone IP.\n"
		"phone_ip=%s\n"
		"tcp_port=%d\n"
		"\n"
		"# 1 = download missing map tiles over Wi-Fi (they are saved to the SD card).\n"
		"online=%d\n"
		"\n"
		"# Tile server. Please respect its usage policy. OpenStreetMap's servers\n"
		"# require a real User-Agent and forbid bulk downloading.\n"
		"tile_url=%s\n"
		"user_agent=%s\n"
		"\n"
		"# Where the map starts before the first GPS fix.\n"
		"zoom=%d\n"
		"start_lat=%.5f\n"
		"start_lon=%.5f\n",
		c->web_port, c->udp_port, c->phone_ip, c->tcp_port, c->online ? 1 : 0,
		c->tile_url, c->user_agent, c->zoom, c->start_lat, c->start_lon);
	fclose(f);
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

void config_load(Config *c, const char *path)
{
	config_defaults(c);
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);

	FILE *f = fopen(path, "r");
	if (!f) { config_save(c, path); return; }

	char line[400];
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (!*s || *s == '#') continue;
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = trim(s), *v = trim(eq + 1);

		if      (!strcmp(k, "web_port"))   c->web_port = atoi(v);
		else if (!strcmp(k, "udp_port"))   c->udp_port = atoi(v);
		else if (!strcmp(k, "tcp_port"))   c->tcp_port = atoi(v);
		else if (!strcmp(k, "phone_ip"))   snprintf(c->phone_ip, sizeof(c->phone_ip), "%s", v);
		else if (!strcmp(k, "online"))     c->online = atoi(v) != 0;
		else if (!strcmp(k, "tile_url"))   snprintf(c->tile_url, sizeof(c->tile_url), "%s", v);
		else if (!strcmp(k, "user_agent")) snprintf(c->user_agent, sizeof(c->user_agent), "%s", v);
		else if (!strcmp(k, "zoom"))       c->zoom = atoi(v);
		else if (!strcmp(k, "start_lat"))  c->start_lat = atof(v);
		else if (!strcmp(k, "start_lon"))  c->start_lon = atof(v);
	}
	fclose(f);
	if (c->zoom < 2) c->zoom = 2;
	if (c->zoom > 19) c->zoom = 19;
}

bool state_load(double *lat, double *lon, int *zoom)
{
	FILE *f = fopen(STATE_PATH, "r");
	if (!f) return false;
	bool ok = fscanf(f, "%lf %lf %d", lat, lon, zoom) == 3;
	fclose(f);
	if (ok && (*zoom < 2 || *zoom > 19)) *zoom = 15;
	return ok;
}

void state_save(double lat, double lon, int zoom)
{
	FILE *f = fopen(STATE_PATH, "w");
	if (!f) return;
	fprintf(f, "%.6f %.6f %d\n", lat, lon, zoom);
	fclose(f);
}
