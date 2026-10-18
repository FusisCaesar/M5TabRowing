"""Download session recordings from the Tab5's SD card over USB serial.

Usage:
  python tools/fetch.py COM10                  # list the sessions on the card
  python tools/fetch.py COM10 latest           # download the newest session
  python tools/fetch.py COM10 7 9              # download sessions 7 and 9
  python tools/fetch.py COM10 all --out D:/rowing

Files land in recordings/ by default (S0007_imu.csv, S0007_strokes.csv, ...). Files that
are already there with the same size are skipped, so re-running only fetches what's new.
Stop the session on the Tab5 first: downloads are refused while it is recording.

Each 4 KB chunk carries a CRC32 and is re-requested if bytes went missing on the way.
"""
import argparse
import os
import re
import sys
import time
import zlib

from tab5serial import open_port

CHUNK = 4096
# S0007_imu.csv, or S0007_Bosbaan_intervals_imu.csv once the session was renamed on the Tab5.
FILE_RE = re.compile(r"\[files\]\s+(?:.*/)?(S(\d{4})_[\w-]+\.csv)\s+(\d+) bytes")


def list_files(ser):
    """Returns {session: [(name, size), ...]} from the device's 'D' listing."""
    for _ in range(3):
        ser.reset_input_buffer()
        ser.write(b"D")
        sessions = {}
        quiet_until = time.time() + 6
        while time.time() < quiet_until:
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if line.startswith("[files] no SD"):
                sys.exit("[fetch] the Tab5 reports no SD card")
            m = FILE_RE.search(line)
            if m:
                sessions.setdefault(int(m.group(2)), []).append((m.group(1), int(m.group(3))))
                quiet_until = time.time() + 1.5
        if sessions:
            return sessions
    return {}


def fetch_file(ser, remote, local, size):
    ser.reset_input_buffer()
    ser.write(f"B {remote}\n".encode("ascii"))
    deadline = time.time() + 10
    while True:
        if time.time() > deadline:
            raise RuntimeError("no answer from the device")
        line = ser.readline().decode("utf-8", "replace").strip()
        if line.startswith("[fetch] ERR"):
            raise RuntimeError(line)
        if line.startswith("[fetch] SIZE"):
            size = int(line.split()[2])
            break

    data = bytearray()
    cmd, retries, t0 = b"N", 0, time.time()
    while len(data) < size:
        want = min(CHUNK, size - len(data))
        ser.write(cmd)
        got = ser.read(want + 4)
        if len(got) == want + 4 and zlib.crc32(got[:want]) == int.from_bytes(got[want:], "little"):
            data += got[:want]
            cmd, retries = b"N", 0
            if len(data) % (CHUNK * 64) == 0 or len(data) == size:
                rate = len(data) / max(time.time() - t0, 0.001) / 1024
                print(f"\r  {local}: {100 * len(data) / size:5.1f}%  {rate:5.0f} KB/s", end="", flush=True)
            continue
        retries += 1
        if retries > 20:
            ser.write(b"Q")
            raise RuntimeError(f"too many bad chunks at byte {len(data)}")
        time.sleep(0.3)
        ser.reset_input_buffer()
        cmd = b"R"
    ser.write(b"Q")
    with open(local, "wb") as f:
        f.write(data)
    print(f"\r  {local}: {size} bytes in {time.time() - t0:.1f} s" + " " * 20)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("which", nargs="*", help="session numbers, 'latest' or 'all'; empty lists the card")
    ap.add_argument("--out", default="recordings", help="destination folder (default: recordings)")
    args = ap.parse_args()

    with open_port(args.port, timeout=3) as ser:
        sessions = list_files(ser)
        if not sessions:
            sys.exit("[fetch] no session files on the card")
        if not args.which:
            for s in sorted(sessions):
                total = sum(size for _, size in sessions[s])
                files = [n.removesuffix(".csv").split("_") for n, _ in sorted(sessions[s])]
                label = " ".join(files[0][1:-1])
                streams = ", ".join(f[-1] for f in files)
                print(f"S{s:04d}  {total / 1e6:7.2f} MB  {label:<26} ({streams})")
            return

        if "all" in args.which:
            wanted = sorted(sessions)
        elif "latest" in args.which:
            wanted = [max(sessions)]
        else:
            wanted = [int(w) for w in args.which]
        os.makedirs(args.out, exist_ok=True)
        for s in wanted:
            if s not in sessions:
                print(f"S{s:04d}: not on the card")
                continue
            print(f"S{s:04d}")
            for name, size in sorted(sessions[s]):
                local = os.path.join(args.out, name)
                if os.path.exists(local) and os.path.getsize(local) == size:
                    print(f"  {local}: already here")
                    continue
                fetch_file(ser, f"/rowing/{name}", local, size)


if __name__ == "__main__":
    main()
