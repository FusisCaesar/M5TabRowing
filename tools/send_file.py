"""Upload a file to the Tab5's SD card over USB serial.

Usage: python tools/send_file.py COM10 maps/utrecht.rmap /map/utrecht.rmap

Sends a header line "U <dest> <size> <crc32hex>", waits for READY, then sends the
file in small chunks, waiting for an ACK byte after each so the device's USB
buffer never overflows. The Tab5 writes the bytes to the SD card and verifies the
CRC. Refuses while a session is recording.
"""
import sys
import time
import zlib

from tab5serial import open_port

CHUNK = 240
ACK = 0x06


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: send_file.py PORT LOCALFILE [DESTPATH]")
    port = sys.argv[1]
    local = sys.argv[2]
    dest = sys.argv[3] if len(sys.argv) > 3 else "/map/" + local.replace("\\", "/").split("/")[-1]

    with open(local, "rb") as fh:
        data = fh.read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    header = f"U {dest} {len(data)} {crc:08x}\n".encode("ascii")

    with open_port(port, timeout=5) as ser:
        ser.reset_input_buffer()
        ser.write(header)
        ser.flush()

        deadline = time.time() + 10
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if "[upload] READY" in line:
                break
            if "[upload] ERR" in line:
                print(line)
                sys.exit(1)
        else:
            sys.exit("[send_file] device did not become ready")

        t0 = time.time()
        for off in range(0, len(data), CHUNK):
            ser.write(data[off:off + CHUNK])
            ser.flush()
            ack = ser.read(1)
            if not ack or ack[0] != ACK:
                sys.exit(f"[send_file] no ACK at byte {off} (got {ack!r})")
            if off % (CHUNK * 200) == 0:
                pct = 100 * off / len(data)
                print(f"\r[send_file] {pct:5.1f}%", end="", flush=True)
        print(f"\r[send_file] sent {len(data)} bytes in {time.time() - t0:.1f}s, waiting for result...")

        deadline = time.time() + 15
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            print(line)
            if "[upload] OK" in line:
                return
            if "[upload] ERR" in line:
                sys.exit(1)
        sys.exit("[send_file] no result from device")


if __name__ == "__main__":
    main()
