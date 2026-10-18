"""Send commands to the Tab5 (N next screen, G start, X stop, L lap, R reset).

Usage: python tools/send.py COM10 G
       python tools/send.py COM10 "W MyNetwork|MyPassword"
       python tools/send.py COM10 "T 640 360"          # tap (P: hold, Z x0 y0 x1 y1: drag)

Commands with arguments end in a newline. For W the device's reply is printed until
it reports OK or ERR (up to 5 minutes); touch commands have no reply.
"""
import re
import sys
import time

from tab5serial import open_port

cmd = sys.argv[2]
with open_port(sys.argv[1], timeout=0.5) as ser:
    if len(cmd) == 1:
        ser.write(cmd.encode("utf-8"))
        ser.flush()
        sys.exit(0)

    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("utf-8"))
    ser.flush()
    if cmd[0] in "TPZ":
        sys.exit(0)
    deadline = time.time() + 300
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if not line or line.startswith(("[status]", "[stroke]")):
            continue
        print(line)
        if re.search(r"^\[\w+\] (OK|ERR)\b", line):
            sys.exit(0 if " OK" in line else 1)
    sys.exit("no OK/ERR reply from the device within 5 minutes")
