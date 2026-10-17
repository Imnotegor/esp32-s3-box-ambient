"""95-second serial smoke test: version, Wi-Fi status, three quiet test tones."""
import argparse
import os
import re
import time
from pathlib import Path

import serial

parser = argparse.ArgumentParser()
parser.add_argument("tag")
args = parser.parse_args()
if not re.fullmatch(r"[A-Za-z0-9_-]+", args.tag):
    parser.error("Tag must be a simple log filename component")
if not os.environ.get("BOX3_SERIAL_PORT"):
    parser.error("BOX3_SERIAL_PORT is required")
os.umask(0o077)
root = Path.home() / ".local/state/box3-diagnostics"
root.mkdir(parents=True, exist_ok=True, mode=0o700)
root.chmod(0o700)
path = root / f"{args.tag}-soak.log"
if path.exists():
    raise SystemExit("Output already exists; choose a new tag")
commands = [(1, "kernel version"), (4, "wifi status"),
            (8, "box3_beep"), (30, "box3_beep"), (55, "box3_beep")]
with serial.Serial(os.environ["BOX3_SERIAL_PORT"], 115200, timeout=0.2) as port, path.open("wb") as log:
    start = time.monotonic()
    while time.monotonic() - start < 95:
        elapsed = time.monotonic() - start
        if commands and elapsed >= commands[0][0]:
            _, command = commands.pop(0)
            port.write((command + "\r").encode())
            port.flush()
            print("SENT", command, flush=True)
        data = port.read(2048)
        if data:
            log.write(data)
            log.flush()
print("SOAK_CAPTURE_COMPLETE")
