#!/usr/bin/env python3
"""
Test 3DS Maps without going outside: sends a fake GPS position walking in a
circle, from a PC on the same Wi-Fi as the 3DS.

  python3 fake_gps.py 192.168.1.50                 # 3DS IP (bottom screen)
  python3 fake_gps.py 192.168.1.50 40.4168 -3.7038 # circle around a point
"""
import math
import socket
import sys
import time
from datetime import datetime, timezone


def nmea(body):
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"${body}*{cs:02X}\r\n"


def dm(v, is_lat):
    h = ("N" if v >= 0 else "S") if is_lat else ("E" if v >= 0 else "W")
    v = abs(v)
    d = int(v)
    m = (v - d) * 60
    return (f"{d:02d}{m:07.4f}" if is_lat else f"{d:03d}{m:07.4f}"), h


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ip = sys.argv[1]
    clat = float(sys.argv[2]) if len(sys.argv) > 2 else 40.4168
    clon = float(sys.argv[3]) if len(sys.argv) > 3 else -3.7038
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    r, t0 = 0.003, time.time()
    while True:
        a = (time.time() - t0) / 60 * 2 * math.pi   # one lap per minute
        lat, lon = clat + r * math.sin(a), clon + r * math.cos(a) / math.cos(math.radians(clat))
        course = (math.degrees(-a)) % 360
        now = datetime.now(timezone.utc)
        t, d = now.strftime("%H%M%S.00"), now.strftime("%d%m%y")
        la, ns = dm(lat, True)
        lo, ew = dm(lon, False)
        msg = nmea(f"GPRMC,{t},A,{la},{ns},{lo},{ew},20.0,{course:.1f},{d},,,A") + \
              nmea(f"GPGGA,{t},{la},{ns},{lo},{ew},1,09,0.9,650.0,M,0.0,M,,")
        s.sendto(msg.encode(), (ip, 10110))
        print(f"\r{lat:.5f}, {lon:.5f}", end="", flush=True)
        time.sleep(1)


if __name__ == "__main__":
    main()
