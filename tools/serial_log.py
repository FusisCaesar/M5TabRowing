"""Capture the Tab5's USB serial output for a fixed time, optionally sending commands.

Usage: python tools/serial_log.py COM10 60 [outfile] [--send "3:G,20:L,40:X"]
  --send schedules single-character commands at the given seconds after start.
"""
import argparse
import time

import serial

from tab5serial import open_port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM10")
    ap.add_argument("seconds", nargs="?", type=float, default=30)
    ap.add_argument("outfile", nargs="?")
    ap.add_argument("--send", default="")
    args = ap.parse_args()

    schedule = []
    for item in filter(None, args.send.split(",")):
        at, cmd = item.split(":")
        schedule.append((float(at), cmd))
    schedule.sort()

    out = open(args.outfile, "w", encoding="utf-8") if args.outfile else None
    start = time.time()
    deadline = start + args.seconds
    ser = None
    while ser is None and time.time() < deadline:
        try:
            ser = open_port(args.port, timeout=0.2)
        except serial.SerialException:
            time.sleep(0.5)
    if ser is None:
        raise SystemExit(f"could not open {args.port}")

    while time.time() < deadline:
        while schedule and time.time() - start >= schedule[0][0]:
            ser.write(schedule.pop(0)[1].encode("ascii"))
        try:
            line = ser.readline()
        except serial.SerialException:
            ser.close()
            time.sleep(0.5)
            try:
                ser = open_port(args.port, timeout=0.2)
            except serial.SerialException:
                pass
            continue
        if line:
            text = f"{time.strftime('%H:%M:%S')} {line.decode('utf-8', 'replace').rstrip()}"
            print(text, flush=True)
            if out:
                out.write(text + "\n")
    ser.close()


if __name__ == "__main__":
    main()
