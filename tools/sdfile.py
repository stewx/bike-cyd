"""List or fetch files on the CYD's SD card over serial.

    python tools/sdfile.py ls /probe
    python tools/sdfile.py get /probe/frames-123.log [out.log]

The port is opened without toggling DTR/RTS so the board is not reset.
"""

import argparse
import base64
import re
import time

import serial

DATA = re.compile(rb"D(\d+) ([A-Za-z0-9+/=]+) ([0-9a-f]{4})")


def open_port(port):
    p = serial.Serial()
    p.port = port
    p.baudrate = 115200
    p.timeout = 5
    p.dtr = False
    p.rts = False
    p.open()
    p.reset_input_buffer()
    return p


def wait_for(p, prefix, seconds=10):
    deadline = time.time() + seconds
    while time.time() < deadline:
        line = p.readline().strip()
        if line.startswith(prefix):
            return line
    raise SystemExit(f"no {prefix.decode()} reply from the board")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["ls", "get"])
    ap.add_argument("path")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", default="COM3")
    args = ap.parse_args()

    p = open_port(args.port)
    p.write(f":{args.action} {args.path}\n".encode())

    if args.action == "ls":
        wait_for(p, b"LIST ")
        while True:
            line = p.readline().strip()
            if line == b"EOF" or not line:
                break
            if line.startswith(b"F "):
                print(line[2:].decode())
        return

    header = wait_for(p, b"FILE ")
    size = int(header.split()[-1])
    if size < 0:
        raise SystemExit(f"{args.path}: not found")
    chunks = {}
    deadline = time.time() + 30 + size / 5000
    while time.time() < deadline:
        line = p.readline().strip()
        if line == b"EOF":
            break
        m = DATA.search(line)
        if m:
            data = base64.b64decode(m.group(2))
            if sum(data) & 0xFFFF == int(m.group(3), 16):
                chunks[int(m.group(1))] = data
    p.close()

    out = bytearray()
    for offset in sorted(chunks):
        if offset != len(out):
            raise SystemExit(f"missing data at byte {len(out)}; try again")
        out += chunks[offset]
    if len(out) < size:
        raise SystemExit(f"got {len(out)} of {size} bytes; try again")
    dest = args.out or args.path.rsplit("/", 1)[-1]
    with open(dest, "wb") as f:
        f.write(out)
    print(f"saved {dest} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
