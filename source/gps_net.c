#include "gps_net.h"
#include "web_gps.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int udp_fd = -1;
static int tcp_fd = -1;
static struct sockaddr_in tcp_addr;
static struct sockaddr_in cand[4];   /* phone addresses to try, in turn */
static int n_cand, cand_idx;

static void use_candidate(GpsNet *g)
{
	tcp_addr = cand[cand_idx];
	snprintf(g->tcp_target, sizeof(g->tcp_target), "%s:%d",
	         inet_ntoa(tcp_addr.sin_addr), ntohs(tcp_addr.sin_port));
}
static bool tcp_enabled;
static uint64_t tcp_retry_at;
static NmeaStream udp_stream, tcp_stream;

/* --- tiny HTTP server for the phone-browser GPS page --- */
#define MAX_HTTP 4
typedef struct { int fd; int len; uint64_t started; char buf[2048]; } HttpClient;
static int http_fd = -1;
static HttpClient http[MAX_HTTP];

static void set_nonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void gps_net_init(GpsNet *g, const Config *cfg)
{
	memset(g, 0, sizeof(*g));
	nmea_init(&g->fix);
	nmea_stream_init(&udp_stream);
	nmea_stream_init(&tcp_stream);
	strcpy(g->source, "-");

	struct in_addr ip = {0}, mask = {0}, bcast = {0};
	SOCU_GetIPInfo(&ip, &mask, &bcast);
	snprintf(g->my_ip, sizeof(g->my_ip), "%s", inet_ntoa(ip));

	/* UDP listener: phone apps / scripts push NMEA to <3DS ip or broadcast>:udp_port */
	udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp_fd >= 0) {
		struct sockaddr_in a = {0};
		a.sin_family = AF_INET;
		a.sin_port = htons(cfg->udp_port);
		a.sin_addr.s_addr = INADDR_ANY;
		if (bind(udp_fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
			close(udp_fd);
			udp_fd = -1;
		} else {
			set_nonblock(udp_fd);
		}
	}

	/* HTTP server: phone opens http://<3DS ip>:web_port in its browser */
	for (int i = 0; i < MAX_HTTP; i++) http[i].fd = -1;
	if (cfg->web_port > 0) {
		http_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (http_fd >= 0) {
			struct sockaddr_in a = {0};
			a.sin_family = AF_INET;
			a.sin_port = htons(cfg->web_port);
			a.sin_addr.s_addr = INADDR_ANY;
			if (bind(http_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(http_fd, 4) < 0) {
				close(http_fd);
				http_fd = -1;
			} else {
				set_nonblock(http_fd);
				g->web_port = cfg->web_port;
			}
		}
	}

	/* TCP client: the 3DS connects to an NMEA server running on the phone */
	tcp_enabled = false;
	g->tcp_state = TCP_OFF;
	if (cfg->phone_ip[0] && strcmp(cfg->phone_ip, "off") != 0) {
		uint32_t hosts[2];
		int nh = 0;
		if (strcmp(cfg->phone_ip, "auto") == 0) {
			/* On a phone hotspot the phone is the gateway (almost always .1).
			 * 127.0.0.1 covers an emulator running on the phone itself. */
			if (ip.s_addr) hosts[nh++] = (ip.s_addr & mask.s_addr) | htonl(1);
			hosts[nh++] = htonl(0x7F000001);
		} else {
			struct in_addr a;
			if (inet_aton(cfg->phone_ip, &a)) hosts[nh++] = a.s_addr;
		}
		/* try the configured port, plus Share GPS's default when auto-detecting */
		int ports[2] = { cfg->tcp_port, 50000 };
		int np = (strcmp(cfg->phone_ip, "auto") == 0 && cfg->tcp_port != 50000) ? 2 : 1;
		n_cand = 0;
		for (int pi = 0; pi < np; pi++)
			for (int hi = 0; hi < nh; hi++) {
				struct sockaddr_in *c = &cand[n_cand++];
				memset(c, 0, sizeof(*c));
				c->sin_family = AF_INET;
				c->sin_port = htons(ports[pi]);
				c->sin_addr.s_addr = hosts[hi];
			}
		if (n_cand > 0) {
			tcp_enabled = true;
			g->tcp_state = TCP_WAITING;
			cand_idx = 0;
			use_candidate(g);
		}
	}
	tcp_retry_at = 0;
}

static void tcp_drop(GpsNet *g)
{
	bool was_connected = g->tcp_state == TCP_CONNECTED;
	if (tcp_fd >= 0) close(tcp_fd);
	tcp_fd = -1;
	g->tcp_state = TCP_WAITING;
	if (!was_connected && n_cand > 1) {
		/* nobody there: try the next address soon */
		cand_idx = (cand_idx + 1) % n_cand;
		use_candidate(g);
		tcp_retry_at = osGetTime() + 1000;
	} else tcp_retry_at = osGetTime() + 3000;
	nmea_stream_init(&tcp_stream);
}

static void note_update(GpsNet *g, const char *src)
{
	uint64_t now = osGetTime();
	g->last_rx_ms = now;
	if (g->fix.valid) g->last_fix_ms = now;
	snprintf(g->source, sizeof(g->source), "%s", src);
}

static void send_all(int fd, const char *p, int n)
{
	uint64_t until = osGetTime() + 2000;
	while (n > 0 && osGetTime() < until) {
		int w = send(fd, p, n, 0);
		if (w > 0) { p += w; n -= w; }
		else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) svcSleepThread(1000000);
		else break;
	}
}

static void http_reply(int fd, const char *status, const char *type, const char *body, int len)
{
	char hdr[256];
	int h = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, len);
	send_all(fd, hdr, h);
	if (len) send_all(fd, body, len);
}

/* Returns true if a position was received. */
static bool http_handle(GpsNet *g, HttpClient *c)
{
	c->buf[c->len] = 0;
	char *end = strstr(c->buf, "\r\n\r\n");
	if (!end) return false;                      /* headers incomplete */
	char *body = end + 4;
	int clen = 0;
	char *cl = strstr(c->buf, "Content-Length:");
	if (!cl) cl = strstr(c->buf, "content-length:");
	if (cl && cl < end) clen = atoi(cl + 15);
	if (clen < 0) clen = 0;
	if (body + clen > c->buf + c->len) return false;   /* body incomplete */
	body[clen] = 0;

	bool got = false;
	if (!strncmp(c->buf, "POST /pos", 9)) {
		got = web_gps_parse(&g->fix, body);
		http_reply(c->fd, got ? "200 OK" : "400 Bad Request", "text/plain", got ? "ok" : "bad", got ? 2 : 3);
		if (got) {
			struct sockaddr_in peer;
			socklen_t pl = sizeof(peer);
			char src[32] = "browser";
			if (getpeername(c->fd, (struct sockaddr *)&peer, &pl) == 0)
				snprintf(src, sizeof(src), "Web %s", inet_ntoa(peer.sin_addr));
			note_update(g, src);
		}
	} else if (!strncmp(c->buf, "GET / ", 6) || !strncmp(c->buf, "GET /index", 10)) {
		http_reply(c->fd, "200 OK", "text/html; charset=utf-8", WEB_GPS_PAGE, (int)strlen(WEB_GPS_PAGE));
	} else {
		http_reply(c->fd, "404 Not Found", "text/plain", "not found", 9);
	}
	close(c->fd);
	c->fd = -1;
	return got;
}

static bool http_poll(GpsNet *g)
{
	bool got = false;
	if (http_fd < 0) return false;
	uint64_t now = osGetTime();
	for (;;) {
		int fd = accept(http_fd, NULL, NULL);
		if (fd < 0) break;
		int i;
		for (i = 0; i < MAX_HTTP && http[i].fd >= 0; i++) ;
		if (i == MAX_HTTP) { close(fd); break; }
		set_nonblock(fd);
		http[i].fd = fd;
		http[i].len = 0;
		http[i].started = now;
	}
	for (int i = 0; i < MAX_HTTP; i++) {
		HttpClient *c = &http[i];
		if (c->fd < 0) continue;
		int n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, 0);
		if (n > 0) c->len += n;
		if ((n == 0) || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ||
		    now - c->started > 5000 || c->len >= (int)sizeof(c->buf) - 1) {
			if (c->len >= (int)sizeof(c->buf) - 1 || n == 0) got |= http_handle(g, c);
			if (c->fd >= 0) { close(c->fd); c->fd = -1; }
			continue;
		}
		if (n > 0) got |= http_handle(g, c);
	}
	return got;
}

bool gps_net_poll(GpsNet *g)
{
	bool changed = false;
	char buf[1500];

	if (udp_fd >= 0) {
		for (int i = 0; i < 16; i++) {
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			int n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
			if (n <= 0) break;
			int u = nmea_stream_feed(&udp_stream, &g->fix, buf, n);
			u += nmea_stream_feed(&udp_stream, &g->fix, "\n", 1); /* datagram = complete lines */
			if (u) {
				char src[32];
				snprintf(src, sizeof(src), "UDP %s", inet_ntoa(from.sin_addr));
				note_update(g, src);
				changed = true;
			}
		}
	}

	if (http_poll(g)) changed = true;

	if (tcp_enabled) {
		uint64_t now = osGetTime();
		if (tcp_fd < 0 && now >= tcp_retry_at) {
			tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
			if (tcp_fd >= 0) {
				set_nonblock(tcp_fd);
				int r = connect(tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr));
				if (r == 0) g->tcp_state = TCP_CONNECTED;
				else if (errno == EINPROGRESS || errno == EALREADY) {
					g->tcp_state = TCP_CONNECTING;
					tcp_retry_at = now + 5000;  /* connect timeout */
				} else tcp_drop(g);
			} else tcp_retry_at = now + 3000;
		} else if (tcp_fd >= 0 && g->tcp_state == TCP_CONNECTING) {
			/* Re-issuing connect() on a non-blocking socket reports progress. */
			int r = connect(tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr));
			if (r == 0 || errno == EISCONN) g->tcp_state = TCP_CONNECTED;
			else if (errno != EINPROGRESS && errno != EALREADY) tcp_drop(g);
			else if (now >= tcp_retry_at) tcp_drop(g);
		}
		if (tcp_fd >= 0 && g->tcp_state == TCP_CONNECTED) {
			for (int i = 0; i < 8; i++) {
				int n = recv(tcp_fd, buf, sizeof(buf), 0);
				if (n > 0) {
					if (nmea_stream_feed(&tcp_stream, &g->fix, buf, n)) {
						note_update(g, "TCP");
						changed = true;
					}
				} else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
					tcp_drop(g);
					break;
				} else break;
			}
		}
	}
	return changed;
}

void gps_net_exit(GpsNet *g)
{
	if (udp_fd >= 0) close(udp_fd);
	if (tcp_fd >= 0) close(tcp_fd);
	if (http_fd >= 0) close(http_fd);
	for (int i = 0; i < MAX_HTTP; i++) if (http[i].fd >= 0) close(http[i].fd);
	udp_fd = tcp_fd = http_fd = -1;
}
