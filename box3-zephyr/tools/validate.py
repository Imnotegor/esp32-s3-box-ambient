"""Validate observed bring-up logs; SD/network limitations remain explicit."""
import hashlib
import json
import math
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def read_log(name):
    text = (root / name).read_text(errors="replace")
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    return text.replace("uart:~$ ", "")


boot = read_log("release-runtime.log")
soak = read_log("release-soak.log")
assert "Zephyr OS build 03b0b5443c7a" in boot
assert "display_ready=1 dock_bus_ready=1 touch_ready=1" in boot
assert "SPI SRAM memory test OK" in boot
assert "BOX3_AUDIO codec_init=0" in boot
assert "BOX3_AUDIO i2s_configure=0" in boot
assert "BOX3_AUDIO i2s_start=0" in boot
assert "BOX3_IMU init=0" in boot
assert "expected=0x60 ICM42607P" in boot
assert "BOX3_BLE advertising_start=0" in boot
assert "Hash of data verified" in (root / "release-flash.log").read_text()
for bad in ["BOX3_AUDIO stopped", "BOX3_IMU sample_error", "FATAL ERROR", "ASSERTION FAIL"]:
    assert bad not in boot + soak, bad

wifi = re.search(r"scan_wait=0 status=0 access_points=(\d+)", boot)
ble = re.search(r"scan_stop=0 advertisements=(\d+)", boot)
ir = re.search(r"carrier_start=0 stop=0 loopback_edges=(\d+)", boot)
assert wifi and int(wifi[1]) > 0
assert ble and int(ble[1]) > 0
assert ir and int(ir[1]) >= 2
retries = re.findall(r"BOX3_IMU read_retry_result=(-?\d+)", boot + soak)
assert all(int(result) == 0 for result in retries)
assert "Zephyr version 4.5.0-rc2" in soak
assert "State: DISCONNECTED" in soak
assert soak.count("Queued quiet 500 Hz / 300 ms tone") == 3

samples = re.findall(r"sample=(\d+) uptime_ms=(\d+) result=0 temperature_mC=(-?\d+) "
                     r"humidity_milli_percent=(\d+) CRC=OK touch_ready=1", soak)
assert len(samples) >= 40, len(samples)
uptimes = [int(row[1]) for row in samples]
assert all(b > a for a, b in zip(uptimes, uptimes[1:]))
assert uptimes[-1] - uptimes[0] >= 85000
accel = re.findall(r"accel_mg=(-?\d+),(-?\d+),(-?\d+)", soak)
assert len(accel) >= 40
norms = [math.sqrt(sum(int(v) ** 2 for v in row)) for row in accel]
assert any(900 < norm < 1100 for norm in norms), "No approximately-1g stationary sample"
levels = re.findall(r"BOX3_AUDIO frames=(\d+) peak_L=(\d+) peak_R=(\d+)", soak)
assert len(levels) >= 40
assert all(int(row[0]) > 0 for row in levels)
assert any(int(row[1]) > 300 and int(row[2]) > 300 for row in levels)

sd = re.search(r"BOX3_SD init=(-?\d+)", boot)
report = {
    "zephyr": "4.5.0-rc2 / 03b0b5443c7a",
    "firmware_sha256": hashlib.sha256((root / "build-full/zephyr/zephyr.bin").read_bytes()).hexdigest(),
    "psram_startup_test": "passed",
    "wifi_access_points": int(wifi[1]),
    "wifi_association": "not tested; disconnected; credentials not provided",
    "ble_advertisements_received": int(ble[1]),
    "ble_nonconnectable_advertising": "started; external reception not independently checked",
    "ir_loopback_edges": int(ir[1]),
    "ir_remote_protocols": "not implemented; carrier loopback only",
    "sd_init_result": int(sd[1]) if sd else None,
    "sd_status": "unresolved initialization failure; no writes or formatting attempted",
    "soak_good_sensor_samples": len(samples),
    "soak_observed_span_ms": uptimes[-1] - uptimes[0],
    "imu_samples": len(accel),
    "imu_read_retries": len(retries),
    "audio_windows": len(levels),
    "shell_beep_requests": 3,
    "microphone_peak_max": [max(int(v[1]) for v in levels), max(int(v[2]) for v in levels)],
    "audio_note": "Changing levels around test tones; no raw recording; listening confirmation pending",
    "verdict": "core bring-up passed with explicit SD/network/manual-test follow-ups",
}
(root / "final-validation.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
