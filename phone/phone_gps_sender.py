#!/usr/bin/env python3
"""
Send your Android phone's GPS position to 3DS Maps (runs in Termux).

Setup on the phone (one time):
  1. Install Termux and Termux:API from F-Droid.
  2. In Termux:  pkg install python termux-api
  3. Give Termux:API the Location permission (Android settings -> Apps).

Use:
  1. Turn on the phone's Wi-Fi hotspot (2.4 GHz) and connect the 3DS to it.
  2. Start 3DS Maps on the 3DS.
  3. In Termux:  python phone_gps_sender.py
     (by default it broadcasts, so you don't need the 3DS's IP address.
      If that doesn't work, pass the IP shown on the 3DS bottom screen:
      python phone_gps_sender.py 192.168.43.123)

It sends standard NMEA $GPRMC and $GPGGA sentences over UDP port 10110.
"""
import json
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

PORT = 10110


def nmea(body):
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"${body}*{cs:02X}\r\n"


def dm(value, is_lat):
    hemi = ("N" if value >= 0 else "S") if is_lat else ("E" if value >= 0 else "W")
    value = abs(value)
    deg = int(value)
    minutes = (value - deg) * 60
    return (f"{deg:02d}{minutes:07.4f}" if is_lat else f"{deg:03d}{minutes:07.4f}"), hemi


def sentences(loc):
    now = datetime.now(timezone.utc)
    t = now.strftime("%H%M%S.") + f"{now.microsecond // 10000:02d}"
    d = now.strftime("%d%m%y")
    lat, ns = dm(loc["latitude"], True)
    lon, ew = dm(loc["longitude"], False)
    knots = (loc.get("speed") or 0) * 1.943844          # m/s -> knots
    course = loc.get("bearing")
    course = f"{course:.1f}" if course is not None else ""
    alt = loc.get("altitude") or 0
    acc = loc.get("accuracy") or 5
    hdop = max(acc / 5.0, 0.5)                           # rough: HDOP ~ accuracy / 5 m
    rmc = nmea(f"GPRMC,{t},A,{lat},{ns},{lon},{ew},{knots:.1f},{course},{d},,,A")
    gga = nmea(f"GPGGA,{t},{lat},{ns},{lon},{ew},1,08,{hdop:.1f},{alt:.1f},M,0.0,M,,")
    return rmc + gga


def location_stream():
    """Yield location dicts from 'termux-location -r updates' (pretty-printed JSON)."""
    p = subprocess.Popen(["termux-location", "-p", "gps", "-r", "updates", "-d", "1000"],
                         stdout=subprocess.PIPE, text=True)
    buf, depth = "", 0
    for line in p.stdout:
        depth += line.count("{") - line.count("}")
        buf += line
        if depth == 0 and buf.strip():
            try:
                yield json.loads(buf)
            except json.JSONDecodeError:
                pass
            buf = ""


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "255.255.255.255"
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    print(f"Sending GPS to {target}:{PORT}. Waiting for a fix (go outside)... Ctrl+C to stop.")
    n = 0
    for loc in location_stream():
        if "latitude" not in loc:
            continue
        msg = sentences(loc).encode()
        try:
            sock.sendto(msg, (target, PORT))
        except OSError as ex:
            print("send failed:", ex)
            time.sleep(1)
            continue
        n += 1
        print(f"\r#{n}  {loc['latitude']:.5f}, {loc['longitude']:.5f}  ±{loc.get('accuracy', 0):.0f} m   ",
              end="", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
