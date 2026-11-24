# 3DS Maps

A map app for the Nintendo 3DS (homebrew) that uses your **phone as the GPS**.
The 3DS shows OpenStreetMap tiles on the top screen with a blue dot for your
position; the phone sends its location to the 3DS over Wi-Fi.

```
 phone (GPS)  --Wi-Fi hotspot-->  3DS
   sends NMEA position              shows map + dot
                                    downloads map tiles through the hotspot
                                    (or uses tiles already on the SD card)
```

## What you need

* A 3DS with custom firmware and the Homebrew Launcher.
* A phone with a Wi-Fi hotspot. The 3DS only does **2.4 GHz** Wi-Fi with WPA2:
  * iPhone: Settings → Personal Hotspot → turn on **Maximize Compatibility**.
  * Android: hotspot settings → AP band **2.4 GHz**.

## Install

1. Copy `3dsmaps.3dsx` to `SD:/3ds/3dsmaps/3dsmaps.3dsx`
   (the `SD` folder in this package already has that layout, copy it over).
2. Optional: copy a `tiles` folder to `SD:/3ds/3dsmaps/tiles` for offline maps
   (see "Offline maps" below).
3. Connect the 3DS to your phone's hotspot (System Settings → Internet Settings).
4. Open **3DS Maps** from the Homebrew Launcher. The first run creates
   `SD:/3ds/3dsmaps/config.ini`.

## Sending the phone's GPS

The 3DS accepts location three ways at the same time: from a web page in the phone's browser (port **8080**, see Android below), and as standard **NMEA 0183** GPS sentences:

* **UDP push** – the phone sends to the 3DS on port **10110**
  (the 3DS IP is shown on the bottom screen).
* **TCP pull** – the 3DS connects to an NMEA server on the phone
  (`phone_ip=auto` means the hotspot gateway, i.e. the phone itself; port `tcp_port=11123`).

### Android (easiest: no app needed, just Chrome)

The 3DS serves a small web page. Chrome on the phone reads your GPS and sends it to the 3DS.

1. Open 3DS Maps. The bottom screen shows **On phone open: http://<3DS IP>:8080**.
2. One-time Chrome setup. Chrome only gives location to "secure" pages, so allow this one:
   open `chrome://flags/#unsafely-treat-insecure-origin-as-secure`, type the address
   from the 3DS screen (for example `http://192.168.43.57:8080`) in the box,
   set it to **Enabled**, and tap **Relaunch**.
3. Open that address in Chrome, tap **Start sending GPS**, and allow location.
4. Keep the page open with the screen on. The 3DS bottom screen shows **FIX**.

The hotspot can give the 3DS a different IP next time. If the page won't load,
check the address on the 3DS screen and update the Chrome flag if it changed.

Other Android options: an NMEA-over-UDP app pointed at the 3DS IP, port `10110`,
or the Termux script `phone/phone_gps_sender.py`.

### iPhone

iOS can't run scripts like Termux, so use an app that serves NMEA over the
network, for example **GPS2IP**. Set it to TCP/socket mode on port `11123` and
start it; with the iPhone hotspot on, 3DS Maps connects to it automatically
(`phone_ip=auto` → `172.20.10.1`). If your app uses another port, change
`tcp_port` in `config.ini`. Keep the app in the foreground or allowed to run
in the background.

## Controls

| Button | Action |
|---|---|
| Circle Pad / D-Pad / drag on touch screen | Move the map (stops following) |
| R / ZR | Zoom in |
| L / ZL | Zoom out |
| A | Follow GPS again |
| SELECT | Tile downloading on/off |
| X | Search a place and get directions |
| B | End the current route |
| START | Quit (remembers where you were) |

The bottom screen shows: fix status, lat/lon, speed, course, satellites,
the 3DS IP, the phone connection, and tile download status.

## Search and directions

Press **X**, type where you want to go (an address, a street, or a place like
"Prado Museum"), and press Search. Results near you come first and show as pins
on the map. Choose one with Up/Down, then press **A** for car directions or
**Y** for walking directions.

While navigating, the top of the map shows the next turn with an arrow and the
distance to it, and the bottom screen shows the turn after that plus the
distance and time left. The route is redrawn from where you are if you go off
it for a few seconds. Press **B** to end the route.

Search and directions need internet (through the phone hotspot). They use the
free OpenStreetMap services Nominatim (search) and OSRM on routing.openstreetmap.de
(routes, by FOSSGIS). Please use them gently: no automated or bulk requests.

## Map tiles

Tiles are stored as `SD:/3ds/3dsmaps/tiles/{z}/{x}/{y}.png` (256×256 PNG).
With `online=1` the 3DS downloads the tiles you look at through the hotspot
(about 10–30 KB each, uses phone data) and saves them, so places you have
viewed once work offline afterwards.

### Offline maps

`tools/download_tiles.py` (Python 3, no extra packages) fetches an area on a PC:

```
python3 download_tiles.py --lat 40.4168 --lon -3.7038 --radius 1 --zoom 14-16
```

Then copy the `tiles` folder to `SD:/3ds/3dsmaps/tiles`.

**OpenStreetMap usage policy:** OSM's tile servers are donated and their
policy forbids bulk downloads (more than ~250 tiles for offline use), so the
script stops at 250 tiles for openstreetmap.org. For a bigger offline area,
use a tile provider whose terms allow it (`--url` / `tile_url=`, PNG tiles),
or browse the area once on the 3DS with downloading on. Map data
© OpenStreetMap contributors, shown on screen.

## config.ini

```
web_port=8080          # phone browser page, 0 = off
udp_port=10110
phone_ip=auto          # or the phone's IP, or off
tcp_port=11123
online=1
tile_url=https://tile.openstreetmap.org/{z}/{x}/{y}.png
user_agent=3DSMaps/1.0 (Nintendo 3DS homebrew map viewer)
zoom=15
start_lat=40.41680
start_lon=-3.70380
```

## Testing without going outside

`tools/fake_gps.py <3DS-IP>` on a PC on the same Wi-Fi sends a fake position
walking in a circle.

## Building

Needs devkitPro (devkitARM, libctru, and the 3ds-curl, 3ds-mbedtls, 3ds-libpng,
3ds-zlib portlibs). Easiest with Docker:

```
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make
```

PC unit tests (NMEA parser, map drawing, map maths): `make -C tests`.

## Files

```
source/main.c      app loop, input, overlays, status screen
source/render.c    map drawing into the 3DS framebuffer, Web Mercator maths
source/tiles.c     tile cache, SD loading, background downloader (libcurl)
source/gps_net.c   UDP listener + TCP client for NMEA
source/nav.c       search results, routes, turn instructions, progress
source/nav_net.c   background search/route requests
source/nmea.c      NMEA $xxRMC / $xxGGA parser
source/web_gps.c   phone web page + its position format
source/config.c    config.ini and last position
romfs/cacert.pem   CA certificates for HTTPS tile downloads
phone/             Android (Termux) GPS sender
tools/             PC tile downloader, fake GPS sender
tests/             PC unit tests
```
