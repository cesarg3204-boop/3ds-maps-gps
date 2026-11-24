#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nmea.h"
#include "config.h"

typedef enum { TCP_OFF, TCP_CONNECTING, TCP_CONNECTED, TCP_WAITING } TcpState;

typedef struct {
	GpsFix   fix;
	uint64_t last_rx_ms;      /* osGetTime() of the last accepted sentence, 0 = never */
	uint64_t last_fix_ms;     /* last time fix.valid was true */
	char     my_ip[32];
	char     tcp_target[32];
	TcpState tcp_state;
	int      web_port;        /* 0 if the web server isn't running */
	char     source[32];      /* who sent the last data */
} GpsNet;

void gps_net_init(GpsNet *g, const Config *cfg);
/* Non-blocking; call once per frame. Returns true if the fix changed. */
bool gps_net_poll(GpsNet *g);
/* Change the phone address ("auto", an IP, or "off") and reconnect. */
void gps_net_set_phone(GpsNet *g, const char *phone_ip);
void gps_net_exit(GpsNet *g);
