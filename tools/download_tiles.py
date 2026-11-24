#!/usr/bin/env python3
"""
Download map tiles for an area so 3DS Maps works offline.

Copy the resulting "tiles" folder to  SD:/3ds/3dsmaps/tiles  on your 3DS card.

Examples
  # 1.5 km around a point, zoom 13-17 (OpenStreetMap, capped at 250 tiles)
  python3 download_tiles.py --lat 40.4168 --lon -3.7038 --radius 1.5 --zoom 13-17

  # a bounding box from another provider that allows offline use
  python3 download_tiles.py --bbox 40.40,-3.72,40.43,-3.69 --zoom 12-16 \
      --url "https://tile.example.com/{z}/{x}/{y}.png?key=YOURKEY" --max 5000

OpenStreetMap's tile usage policy (https://operations.osmfoundation.org/policies/tiles/)
forbids bulk downloading: downloading significantly more than 250 tiles for
offline use is not allowed. This script enforces that limit when the URL is an
openstreetmap.org server. For bigger areas use a provider whose terms allow it,
or just let the 3DS download tiles as you look at them (online=1).
Only Python 3 is needed, no extra packages.
"""
import argparse
import math
import os
import sys
import time
import urllib.error
import urllib.request

OSM_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
OSM_LIMIT = 250
USER_AGENT = "3DSMaps-tile-downloader/1.0 (personal offline cache for a Nintendo 3DS homebrew map viewer)"


def tile_xy(lat, lon, z):
    n = 2 ** z
    lat = max(min(lat, 85.0511), -85.0511)
    x = int((lon + 180.0) / 360.0 * n)
    r = math.radians(lat)
    y = int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def parse_zoom(s):
    if "-" in s:
        a, b = s.split("-", 1)
        return list(range(int(a), int(b) + 1))
    return [int(s)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--bbox", help="south,west,north,east in degrees")
    g.add_argument("--lat", type=float, help="centre latitude (use with --lon and --radius)")
    ap.add_argument("--lon", type=float)
    ap.add_argument("--radius", type=float, default=1.0, help="km around --lat/--lon (default 1)")
    ap.add_argument("--zoom", default="14-16", help="zoom level or range, e.g. 15 or 12-17 (max 19)")
    ap.add_argument("--url", default=OSM_URL, help="tile URL template with {z} {x} {y}")
    ap.add_argument("--out", default="tiles", help="output folder (default ./tiles)")
    ap.add_argument("--max", type=int, default=OSM_LIMIT, help="refuse if more tiles than this")
    ap.add_argument("--delay", type=float, default=0.5, help="seconds between requests")
    ap.add_argument("--dry-run", action="store_true", help="only count the tiles")
    a = ap.parse_args()

    if a.bbox:
        s, w, n, e = (float(v) for v in a.bbox.split(","))
    else:
        if a.lon is None:
            ap.error("--lat needs --lon")
        dlat = a.radius / 111.32
        dlon = a.radius / (111.32 * max(math.cos(math.radians(a.lat)), 0.01))
        s, n, w, e = a.lat - dlat, a.lat + dlat, a.lon - dlon, a.lon + dlon

    jobs = []
    for z in parse_zoom(a.zoom):
        if not 0 <= z <= 19:
            ap.error("zoom must be 0-19")
        x0, y0 = tile_xy(n, w, z)
        x1, y1 = tile_xy(s, e, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                jobs.append((z, x, y))

    todo = [j for j in jobs if not os.path.exists(os.path.join(a.out, *map(str, j)) + ".png")]
    print(f"{len(jobs)} tiles in area, {len(todo)} not downloaded yet")

    limit = a.max
    if "openstreetmap.org" in a.url and limit > OSM_LIMIT:
        print(f"OpenStreetMap servers: limiting to {OSM_LIMIT} tiles (tile usage policy).")
        limit = OSM_LIMIT
    if len(todo) > limit:
        sys.exit(f"Too many tiles ({len(todo)} > {limit}). Use a smaller area or fewer zoom levels.")
    if a.dry_run:
        return

    ok = 0
    for i, (z, x, y) in enumerate(todo, 1):
        url = a.url.replace("{z}", str(z)).replace("{x}", str(x)).replace("{y}", str(y))
        path = os.path.join(a.out, str(z), str(x), f"{y}.png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
            if not data.startswith(b"\x89PNG"):
                print(f"  {z}/{x}/{y}: not a PNG, skipped (3DS Maps reads PNG tiles)")
                continue
            with open(path + ".part", "wb") as f:
                f.write(data)
            os.replace(path + ".part", path)
            ok += 1
        except urllib.error.HTTPError as ex:
            print(f"  {z}/{x}/{y}: HTTP {ex.code}")
            if ex.code in (403, 429):
                sys.exit("Server refused (403/429). Stopping; please try later or use another provider.")
        except Exception as ex:  # network hiccup: keep going
            print(f"  {z}/{x}/{y}: {ex}")
        print(f"\r{i}/{len(todo)}", end="", flush=True)
        time.sleep(a.delay)
    print(f"\nDone: {ok} tiles saved in '{a.out}'. Copy it to SD:/3ds/3dsmaps/tiles")


if __name__ == "__main__":
    main()
