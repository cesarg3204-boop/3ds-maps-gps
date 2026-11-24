/* Phone-browser GPS: the 3DS serves a small web page; the phone opens it in
 * Chrome, the page reads the phone's location and POSTs it back to the 3DS.
 * This file holds the parts that don't touch sockets (unit-testable). */
#include "web_gps.h"
#include <stdlib.h>
#include <string.h>

static bool field(const char **p, double *out, bool *present)
{
	char *end;
	const char *s = *p;
	*present = false;
	if (*s && *s != ',') {
		*out = strtod(s, &end);
		if (end == s) return false;
		*present = true;
		s = end;
	}
	if (*s == ',') s++;
	else if (*s && *s != '\r' && *s != '\n') return false;
	*p = s;
	return true;
}

bool web_gps_parse(GpsFix *fix, const char *body)
{
	double v[6] = {0};
	bool has[6] = {0};
	const char *p = body;
	for (int i = 0; i < 6; i++)
		if (!field(&p, &v[i], &has[i])) return false;
	if (!has[0] || !has[1]) return false;
	if (v[0] < -90 || v[0] > 90 || v[1] < -180 || v[1] > 180) return false;

	fix->lat = v[0];
	fix->lon = v[1];
	fix->hdop = has[2] ? (v[2] / 5.0 < 0.5 ? 0.5 : v[2] / 5.0) : -1;  /* rough HDOP from accuracy */
	fix->speed_kmh = has[3] ? v[3] * 3.6 : 0;
	fix->course_deg = has[4] ? v[4] : -1;
	if (has[5]) fix->altitude_m = v[5];
	fix->sats = -1;
	fix->valid = true;
	fix->count++;
	return true;
}

const char WEB_GPS_PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>3DS Maps GPS</title><style>"
"body{font-family:sans-serif;background:#16202b;color:#eee;margin:0;padding:20px;text-align:center}"
"h1{font-size:22px}#s{font-size:20px;margin:18px 0;padding:14px;border-radius:10px;background:#2a3a4a}"
".ok{background:#1f6b3a!important}.bad{background:#8a2a2a!important}"
"button{font-size:20px;padding:14px 28px;border-radius:10px;border:0;background:#3d8bfd;color:#fff}"
"#d{font-family:monospace;font-size:15px;line-height:1.6}p{color:#aab;font-size:14px;text-align:left}"
"</style></head><body><h1>3DS Maps &middot; phone GPS</h1>"
"<button id=b>Start sending GPS</button><div id=s>Not started</div><div id=d></div>"
"<p>Keep this page open with the screen on while you use the map.</p>"
"<p id=h style=display:none><b>Location is blocked.</b> Chrome only gives location to secure pages. "
"Open <b>chrome://flags/#unsafely-treat-insecure-origin-as-secure</b>, type <b id=o></b> in the box, "
"set it to <b>Enabled</b>, tap <b>Relaunch</b>, then come back here.</p>"
"<script>"
"var S=document.getElementById('s'),D=document.getElementById('d'),n=0,busy=0,wl=null;"
"document.getElementById('o').textContent=location.origin;"
"function st(t,c){S.textContent=t;S.className=c||''}"
"function blocked(){st('Location not available','bad');document.getElementById('h').style.display='block'}"
"function send(p){if(busy)return;busy=1;var c=p.coords;"
"var b=[c.latitude,c.longitude,c.accuracy,c.speed==null?'':c.speed,c.heading==null||isNaN(c.heading)?'':c.heading,c.altitude==null?'':c.altitude].join(',');"
"fetch('/pos',{method:'POST',body:b}).then(function(){n++;busy=0;st('Sending to 3DS \\u2714 ('+n+')','ok');"
"D.innerHTML=c.latitude.toFixed(5)+', '+c.longitude.toFixed(5)+'<br>\\u00b1'+Math.round(c.accuracy)+' m'})"
".catch(function(){busy=0;st('Can\\'t reach the 3DS. Is 3DS Maps open?','bad')})}"
"function start(){if(!window.isSecureContext||!navigator.geolocation){blocked();return}"
"st('Waiting for GPS fix... (go outside)');"
"navigator.geolocation.watchPosition(send,function(e){if(e.code==1)blocked();else st('GPS: '+e.message,'bad')},"
"{enableHighAccuracy:true,maximumAge:0,timeout:30000});"
"if(navigator.wakeLock)navigator.wakeLock.request('screen').then(function(l){wl=l}).catch(function(){});}"
"document.getElementById('b').onclick=start;"
"document.addEventListener('visibilitychange',function(){if(!document.hidden&&wl&&navigator.wakeLock)navigator.wakeLock.request('screen').then(function(l){wl=l}).catch(function(){})});"
"if(!window.isSecureContext)blocked();"
"</script></body></html>";
