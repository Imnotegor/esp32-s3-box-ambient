"""Flash this BOX-3 only, verify write, and capture bounded USB telemetry."""
import argparse
import hashlib
import os
import re
import subprocess
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[1]
os.umask(0o077)
OUTPUT = Path.home() / ".local/state/box3-diagnostics"
OUTPUT.mkdir(parents=True, exist_ok=True, mode=0o700)
OUTPUT.chmod(0o700)
PORT = os.environ.get("BOX3_SERIAL_PORT")
MAC = os.environ.get("BOX3_DEVICE_MAC", "").lower()

parser = argparse.ArgumentParser()
parser.add_argument("build")
parser.add_argument("tag")
parser.add_argument("--seconds", type=int, default=45)
args = parser.parse_args()
if not PORT or not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", MAC):
    parser.error("Configure BOX3_SERIAL_PORT and a complete BOX3_DEVICE_MAC; identity checking is mandatory")
if not re.fullmatch(r"[A-Za-z0-9_-]+", args.tag):
    parser.error("Tag must be a simple log filename component")
firmware = ROOT / args.build / "zephyr/zephyr.bin"
flash_log = OUTPUT / f"{args.tag}-flash.log"
runtime_log = OUTPUT / f"{args.tag}-runtime.log"
if not firmware.is_file() or flash_log.exists() or runtime_log.exists():
    raise SystemExit("Missing firmware or output already exists; choose a new tag")

base = [str(ROOT / ".venv/bin/esptool"), "--chip", "esp32s3", "--port", PORT,
        "--baud", "115200", "--no-stub"]
identity = subprocess.run(base + ["--before", "default-reset", "--after", "no-reset",
                                  "read-mac"], capture_output=True, text=True, timeout=30)
if identity.returncode or MAC not in identity.stdout.lower():
    raise SystemExit("Device identity failed: " + "response omitted to avoid disclosing device identity")

result = subprocess.run(base + ["--before", "no-reset", "--after", "watchdog-reset",
                               "write-flash", "--flash-size", "16MB", "--flash-mode", "dio",
                               "--flash-freq", "80m", "0x0", str(firmware)],
                        capture_output=True, text=True, timeout=180)
flash_log.write_text(identity.stdout + result.stdout + result.stderr)
if result.returncode or "Hash of data verified" not in result.stdout:
    raise SystemExit("Flash failed; inspect " + str(flash_log))
print("FLASH VERIFIED", hashlib.sha256(firmware.read_bytes()).hexdigest(), flush=True)

connection = None
for _ in range(100):
    try:
        connection = serial.Serial(PORT, 115200, timeout=0.2)
        break
    except serial.SerialException:
        time.sleep(0.1)
if connection is None:
    raise SystemExit("USB port did not return after watchdog reset")
with connection, runtime_log.open("wb") as log:
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        data = connection.read(2048)
        if data:
            log.write(data)
            log.flush()
            print(data.decode(errors="replace"), end="", flush=True)
