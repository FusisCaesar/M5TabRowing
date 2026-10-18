"""Grab the Tab5 screen over USB serial and save it as a PNG.

Usage: python tools/screenshot.py COM10 screen.png
"""
import sys
import time

from PIL import Image

from tab5serial import open_port


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
    out = sys.argv[2] if len(sys.argv) > 2 else "screen.png"

    with open_port(port, timeout=30) as ser:
        ser.reset_input_buffer()
        ser.write(b"S")
        deadline = time.time() + 10
        while time.time() < deadline:
            line = ser.readline().decode("ascii", "replace").strip()
            if line.startswith("SCREENSHOT"):
                break
        else:
            sys.exit("no screenshot header received")

        _, w, h = line.split()
        w, h = int(w), int(h)
        data = ser.read(w * h * 2)
        if len(data) != w * h * 2:
            sys.exit(f"short read: {len(data)} of {w * h * 2} bytes")

    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = (data[2 * i] << 8) | data[2 * i + 1]  # M5GFX returns byte-swapped RGB565
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[i % w, i // w] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    img.save(out)
    print(f"saved {out} ({w}x{h})")


if __name__ == "__main__":
    main()
