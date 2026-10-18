"""Feed a real CPU forecast to the connected BOX over its trusted USB console."""
import argparse
import copy
import ipaddress
import json
import os
from pathlib import Path
import re
import struct
import tempfile
import time
import urllib.parse
import urllib.request

import serial


def point_hash(data):
    value = 2166136261
    for point in data["points"]:
        for byte in struct.pack("<7i", *(point[k] for k in ("m", "t", "p", "lo", "hi", "pl", "ph"))):
            value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return f"{value:08x}"


def drain(connection, log, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        block = connection.read(4096)
        if block:
            log.write(block)
            log.flush()


def transfer(connection, payload, exercise, log, present=True):
    started = time.monotonic()
    def command(text, pattern, timeout=6):
        connection.write((text + "\r").encode())
        connection.flush()
        buffer = bytearray()
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            block = connection.read(connection.in_waiting or 1)
            if block:
                log.write(block); log.flush(); buffer.extend(block)
                match = re.search(pattern + rb"[^\r\n]*[\r\n]", buffer)
                if match:
                    return match.group().decode(errors="replace")
        raise RuntimeError("No expected USB acknowledgement for " + text.split()[0])

    def frame(data, accepted):
        data = copy.deepcopy(data)
        elapsed = int(time.monotonic() - started)
        data["age_secs"] += elapsed
        data["source_age_secs"] += elapsed
        raw = json.dumps(data, separators=(",", ":"), allow_nan=False).encode()
        if len(raw) > 3072:
            raise ValueError("Forecast exceeds the BOX protocol limit")
        command("box3_forecast_begin", rb"FORECAST_BEGIN_OK")
        # A whole command must fit the firmware's 64-byte UART RX ring.
        for start in range(0, len(raw), 16):
            part = raw[start:start + 16]
            response = command("box3_forecast_chunk " + part.hex(), rb"FORECAST_CHUNK_OK \d+")
            count = int(re.search(r"FORECAST_CHUNK_OK (\d+)", response).group(1))
            if count != start + len(part):
                raise RuntimeError("USB transfer byte count differs")
        response = command("box3_forecast_commit", rb"BOX3_FORECAST result=-?\d+")
        result = int(re.search(r"result=(-?\d+)", response).group(1))
        if (result == 0) != accepted:
            raise RuntimeError("Unexpected forecast validation result: " + str(result))

    def verify():
        status = command("box3_forecast_status", rb"FORECAST_STATE ")
        if "ok=1 points=25 local=1" not in status or "hash=" + point_hash(payload) not in status:
            raise RuntimeError("Stored BOX forecast differs from the source")

    frame(payload, True)
    verify()
    if exercise:
        invalid = copy.deepcopy(payload)
        invalid["points"][0]["lo"] = invalid["points"][0]["t"] + 1
        frame(invalid, False); verify()
        invalid = copy.deepcopy(payload); del invalid["points"][0]["t"]
        invalid["points"][0]["lo"] = -500
        invalid["points"][0]["hi"] = 500
        frame(invalid, False); verify()
        print("INVALID_AND_MISSING_FIELDS_REJECTED; prior forecast retained", flush=True)
    if present or exercise:
        command("box3_page 2", rb"UI_PAGE_QUEUED")
        time.sleep(1.2)
        controls = [(0, 24), (1, 24), (1, 48), (0, 48)] if exercise else [(0, 48)]
        for metric, hours in controls:
            command(f"box3_weather {metric} {hours}", rb"BOX3_WEATHER view=\w+ hours=\d+ points=25")
            time.sleep(0.3)
    if exercise:
        # Existing HTTPS dashboard polls must not erase the separate USB forecast.
        until = time.monotonic() + 12
        while time.monotonic() < until:
            block = connection.read(4096)
            if block: log.write(block); log.flush()
        verify()
    print("BOX_FORECAST_OK points=25 hash=" + point_hash(payload), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8790/v1/box-forecast")
    parser.add_argument("--exercise", action="store_true")
    parser.add_argument("--watch", action="store_true")
    parser.add_argument("--interval", type=int, default=600)
    args = parser.parse_args()
    location = urllib.parse.urlsplit(args.url)
    if location.scheme != "http" or not ipaddress.ip_address(location.hostname).is_loopback or location.path != "/v1/box-forecast" or location.username or location.password or location.query:
        parser.error("Use the local CPU engine's loopback /v1/box-forecast URL")
    port = os.environ.get("BOX3_SERIAL_PORT")
    if not port or not 300 <= args.interval <= 3600:
        parser.error("Configure BOX3_SERIAL_PORT; interval must be 300..3600 seconds")
    os.umask(0o077)
    directory = Path.home() / ".local/state/box3-diagnostics"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with tempfile.NamedTemporaryFile(prefix="forecast-usb-", suffix=".log", dir=directory, delete=False) as log, serial.Serial(port, baudrate=115200, timeout=0.1, exclusive=True) as connection:
        print("Private USB log:", log.name, flush=True)
        # Let the USB console finish reconnecting before the first command.
        drain(connection, log, 6)
        connection.write(b"\x03\r")
        connection.flush()
        drain(connection, log, 0.3)
        while True:
            delay = args.interval
            try:
                with opener.open(args.url, timeout=5) as response:
                    raw = response.read(3073)
                if len(raw) > 3072:
                    raise ValueError("Forecast response too large")
                payload = json.loads(raw)
                if payload.get("ok") is not True or len(payload.get("points", [])) != 25:
                    raise ValueError("The CPU forecast is unavailable or stale")
                transfer(connection, payload, args.exercise, log, present=not args.watch)
                args.exercise = False
            except (OSError, ValueError, RuntimeError, serial.SerialException) as error:
                if not args.watch:
                    raise
                print("USB forecast update unavailable:", type(error).__name__, flush=True)
                delay = 30
            if not args.watch:
                return
            drain(connection, log, delay)


if __name__ == "__main__":
    main()
