"""Provision Wi-Fi and TLS identities over local USB without dumping secret replies."""
import argparse
import base64
import getpass
import ipaddress
import os
import re
import sys
import time
from pathlib import Path

import serial

parser = argparse.ArgumentParser()
parser.add_argument("--ssid", default=os.environ.get("BOX3_WIFI_SSID"))
parser.add_argument("--server", default=os.environ.get("BOX3_SERVER_IP"))
parser.add_argument("--port", default=os.environ.get("BOX3_SERIAL_PORT"))
parser.add_argument("--interval", type=int, default=3600)
args = parser.parse_args()
if not all([args.ssid, args.server, args.port]):
    parser.error("Set --ssid, --server and --port or the matching BOX3_* environment settings")
ipaddress.IPv4Address(args.server)
if not 60 <= args.interval <= 86400 or not 1 <= len(args.ssid.encode()) <= 32:
    raise SystemExit("Invalid SSID or interval")
if not sys.stdin.isatty():
    raise SystemExit("Use a local terminal for hidden Wi-Fi password entry")
password = getpass.getpass("Wi-Fi password (hidden; saved only in BOX NVS): ")
if not 8 <= len(password.encode()) <= 63:
    raise SystemExit("Expected an 8–63 byte WPA2 Personal passphrase")
pki = Path.home() / ".local/share/box3-telemetry/pki"
fields = {"ssid": args.ssid.encode(), "psk": password.encode(), "ip": args.server.encode(),
          "ca": (pki / "device-ca.pem").read_bytes(),
          "cert": (pki / "box3.pem").read_bytes(),
          "key": (pki / "box3.key").read_bytes()}
del password
ansi = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def send(port, command, expected):
    port.write((command + "\r").encode("ascii"))
    port.flush()
    data = bytearray()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        data.extend(port.read(1024))
        text = ansi.sub("", data.decode(errors="replace"))
        if expected in text:
            return
    raise SystemExit("Provisioning acknowledgement missing; no raw secret-bearing output saved")


port = serial.Serial()
port.port = args.port
port.baudrate = 115200
port.timeout = 0.1
port.dtr = False
port.rts = False
port.open()
with port:
    port.write(b"\x03\r")
    time.sleep(0.3)
    port.reset_input_buffer()
    port.write(b"shell echo off\r")
    time.sleep(0.3)
    port.reset_input_buffer()
    try:
        send(port, "tlm begin", "TLM_BEGIN_OK")
        for name, value in fields.items():
            for offset in range(0, len(value), 24):
                chunk = base64.b64encode(value[offset:offset + 24]).decode()
                send(port, f"tlm put {name} {offset} {chunk}", "TLM_CHUNK_OK")
            print("Provisioned field:", name, flush=True)
        fields.clear()
        send(port, f"tlm interval {args.interval}", "TLM_INTERVAL_OK")
        send(port, "tlm commit", "TLM_COMMIT_OK")
        print("Configuration committed atomically to BOX NVS; no secret readback", flush=True)
    finally:
        port.write(b"shell echo on\r")
        port.flush()
    send(port, "tlm reboot", "TLM_REBOOT")
    print("BOX reboot requested; waiting for autonomous Wi-Fi/HTTPS startup", flush=True)
