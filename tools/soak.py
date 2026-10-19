"""Soak test: runs simulated rides on the board for hours and checks each one
reaches the NAS, while tracking memory and unexpected restarts.

    python tools/soak.py --hours 3 [--port COM3] [--every 15] [--log soak.log]

Every --every minutes (when no real ride or bike connection is active) it
starts a 3-minute simulated ride; every third one is cut short by a restart
to exercise crash recovery. Simulated rides are named "sim-..." on the NAS.
The port is opened without toggling DTR/RTS so the board is not reset.
"""

import argparse
import re
import time

import serial

SIM_SECONDS = 180
REBOOT_AT_S = 170  # after the 20 s pause, so ~150 s of riding survives
SIM_DONE_S = SIM_SECONDS + 90


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=3)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--every", type=float, default=15, help="minutes between simulated rides")
    ap.add_argument("--log", default="soak.log")
    args = ap.parse_args()

    p = serial.Serial()
    p.port = args.port
    p.baudrate = 115200
    p.timeout = 0.5
    p.dtr = False
    p.rts = False
    p.open()
    log = open(args.log, "a", encoding="utf-8")

    def note(msg):
        line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    end = time.time() + args.hours * 3600
    next_sim = time.time() + 30
    sims = uploads = recovered = planned_restarts = 0
    unexpected = []
    min_heap = None
    link = "?"
    ride_active = False
    sim_start = None
    restart_this = restarted = False

    note(f"soak start: {args.hours} h, a simulated ride every {args.every} min")
    while time.time() < end:
        raw = p.readline().decode("ascii", "replace").strip()
        if raw.startswith("status:"):
            m = re.search(r"minheap=(\d+)", raw)
            if m:
                v = int(m.group(1))
                if min_heap is None or v < min_heap:
                    min_heap = v
                    note(f"new low: {raw}")
        elif raw.startswith("bike: link="):
            link = raw.split()[1].split("=")[1]
            ride_active = "ride active=1" in raw
        elif raw.startswith("bike-cyd") and "starting" in raw:
            reason = re.search(r"reset: ([^)]+)", raw)
            reason = reason.group(1) if reason else "?"
            if restart_this and restarted and reason == "restart":
                note(f"planned restart: {raw}")
            else:
                unexpected.append(raw)
                note(f"UNEXPECTED RESTART: {raw}")
            min_heap = None  # the low-water mark restarts with the board
        elif raw.startswith(("tcx:", "nas: uploaded", "ride: recovering", "Guru", "Backtrace")) or "failed" in raw:
            note(raw)
            if raw.startswith("nas: uploaded sim-"):
                uploads += 1
            if raw.startswith("ride: recovering"):
                recovered += 1

        now = time.time()
        if sim_start is None and now >= next_sim:
            if link == "connected" or ride_active:
                next_sim = now + 60  # someone is using the bike; try later
            else:
                sims += 1
                restart_this = sims % 3 == 0
                restarted = False
                sim_start = now
                p.write(f":sim {SIM_SECONDS}\n".encode())
                note(f"simulated ride {sims} started" + (" (will restart mid-ride)" if restart_this else ""))
        if sim_start is not None:
            if restart_this and not restarted and now - sim_start > REBOOT_AT_S:
                restarted = True
                planned_restarts += 1
                p.write(b":reboot\n")
                note("restarting mid-ride")
            if now - sim_start > SIM_DONE_S:
                sim_start = None
                next_sim = now + args.every * 60

    summary = (
        f"soak done: {sims} simulated rides, {uploads} uploaded, {recovered} recovered after "
        f"{planned_restarts} planned restarts, {len(unexpected)} unexpected restarts"
    )
    note(summary)
    for u in unexpected:
        note("  " + u)
    p.close()


if __name__ == "__main__":
    main()
