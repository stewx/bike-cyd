"""Grab the CYD screen over serial and save it as a PNG.

    python tools/screenshot.py [out.png] [--port COM3] [--scale 2]

The firmware answers 'S' with "SHOT w h", then one line per row,
"R<y> <base64 RGB888> <byte sum as 4 hex digits>", then "END". Other log
lines mixed into the stream are ignored. The port is opened without
toggling DTR/RTS so the board is not reset.
"""

import argparse
import base64
import re
import time

import serial
from PIL import Image

ROW = re.compile(rb"R(\d+) ([A-Za-z0-9+/=]+) ([0-9a-f]{4})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="screen.png")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--scale", type=int, default=1)
    args = ap.parse_args()

    p = serial.Serial()
    p.port = args.port
    p.baudrate = 115200
    p.timeout = 5
    p.dtr = False
    p.rts = False
    p.open()
    p.reset_input_buffer()
    p.write(b"S")

    deadline = time.time() + 10
    header = b""
    while not header.startswith(b"SHOT "):
        if time.time() > deadline:
            raise SystemExit("no SHOT header from the board")
        header = p.readline()
    w, h = (int(v) for v in header.split()[1:3])

    rows = {}
    deadline = time.time() + 120
    while time.time() < deadline:
        line = p.readline().strip()
        if line == b"END":
            break
        m = ROW.search(line)
        if not m:
            continue
        data = base64.b64decode(m.group(2))
        if len(data) == w * 3 and sum(data) & 0xFFFF == int(m.group(3), 16):
            rows[int(m.group(1))] = data
    p.close()

    missing = [y for y in range(h) if y not in rows]
    if missing:
        print(f"{len(missing)} rows lost or corrupted (shown magenta)")
    blank = bytes([255, 0, 255]) * w
    img = Image.frombytes("RGB", (w, h), b"".join(rows.get(y, blank) for y in range(h)))
    if args.scale > 1:
        img = img.resize((w * args.scale, h * args.scale), Image.NEAREST)
    img.save(args.out)
    print(f"saved {args.out} ({w}x{h})")


if __name__ == "__main__":
    main()
