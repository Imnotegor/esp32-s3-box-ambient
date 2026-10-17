#!/usr/bin/env python3
"""tmux renders only a local cache; a finite LaunchAgent performs mTLS fetches."""
import argparse
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

CONFIG = Path.home() / ".config/box3-telemetry/client.json"


def valid_snapshot(data, now):
    if not isinstance(data, dict) or not isinstance(data.get("measurement"), dict):
        return False
    m = data["measurement"]
    required = ["temperature_mc", "humidity_milli_percent", "interval_secs"]
    if any(type(m.get(k)) is not int for k in required):
        return False
    received = data.get("received_at_unix")
    return (type(received) is int and 0 < received <= now + 300
            and m.get("node_id") == "box3"
            and -40000 <= m["temperature_mc"] <= 85000
            and 0 <= m["humidity_milli_percent"] <= 100000
            and 60 <= m["interval_secs"] <= 86400)


def read_cache(path):
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def atomic_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, name = tempfile.mkstemp(prefix=".box3-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(data, f)
            f.write("\n")
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def refresh(config):
    path = Path(config["cache"])
    cache = read_cache(path)
    now = int(time.time())
    command = [
        "/usr/bin/curl", "--silent", "--show-error", "--fail",
        "--connect-timeout", "3", "--max-time", "8", "--max-filesize", "4096",
        "--proto", "=https", "--noproxy", "",
        "--socks5-hostname", config["proxy"],
        "--cacert", config["ca"], "--cert", config["cert"], "--key", config["key"],
        config["url"],
    ]
    try:
        result = subprocess.run(command, capture_output=True, timeout=10)
        data = json.loads(result.stdout) if result.returncode == 0 else None
        if not valid_snapshot(data, now):
            raise ValueError("unavailable or invalid response")
        cache.update(snapshot=data, last_success_unix=now, last_error=None)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        cache["last_error"] = "unavailable"
    cache["last_attempt_unix"] = now
    atomic_json(path, cache)


def render(cache, now):
    data = cache.get("snapshot")
    if not valid_snapshot(data, now):
        return "#[fg=#6272a4]BOX --"
    m = data["measurement"]
    age = max(0, now - data["received_at_unix"])
    text = "BOX {:.1f}°C {:.0f}%".format(
        m["temperature_mc"] / 1000, m["humidity_milli_percent"] / 1000)
    duration = "{}m".format(age // 60) if age < 3600 else "{}h".format(age // 3600)
    if age > m["interval_secs"] * 3 // 2 + 120:
        return "#[fg=#6272a4]{} stale {}".format(text, duration)
    if cache.get("last_error"):
        return "#[fg=#f1fa8c]{} cache {}".format(text, duration)
    return "#[fg=#50fa7b]{} {}".format(text, duration)


def render_weather(cache, now):
    snapshot = cache.get("snapshot")
    if not valid_snapshot(snapshot, now):
        return ""
    data = snapshot.get("dashboard")
    if not isinstance(data, dict) or data.get("weather_ok") is not True:
        return ""
    names = ["weather_temp_mc", "weather_feels_mc", "weather_kind", "weather_age_secs"]
    if any(type(data.get(name)) is not int for name in names):
        return ""
    if (not -80000 <= data["weather_temp_mc"] <= 65000
            or not -80000 <= data["weather_feels_mc"] <= 65000
            or not 0 <= data["weather_kind"] <= 6 or data["weather_age_secs"] < 0):
        return ""
    seen = cache.get("last_success_unix")
    seen = seen if type(seen) is int else now
    age = data["weather_age_secs"] + max(0, now - seen)
    old = data.get("weather_stale") is True or age > 1800 or cache.get("last_error")
    icon = ["☀", "⛅", "☂", "❄", "⚡", "⛅", "⛅"][data["weather_kind"]]
    return "{} {:+d}°C {:+d}°C{}".format(icon, data["weather_temp_mc"] // 1000,
            data["weather_feels_mc"] // 1000, " ~" if old else "")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["refresh", "render", "weather"])
    args = parser.parse_args()
    try:
        config = json.loads(CONFIG.read_text())
        path = Path(config["cache"])
    except (OSError, ValueError, KeyError):
        if args.mode == "render":
            print("#[fg=#6272a4]BOX --", end="")
        return
    if args.mode == "refresh":
        refresh(config)
    else:
        cache = read_cache(path)
        renderer = render_weather if args.mode == "weather" else render
        print(renderer(cache, int(time.time())), end="")


if __name__ == "__main__":
    main()
