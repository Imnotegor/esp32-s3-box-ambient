"""Connect using specifically selected local Keychain accounts, without printing passwords."""
import argparse
import subprocess

import serial
from wifi_connect import connect

parser = argparse.ArgumentParser()
parser.add_argument("--ssid", required=True)
parser.add_argument("--account", action="append", required=True)
args = parser.parse_args()

try:
    for account in args.account:
        print("Checking selected saved Wi-Fi credential", flush=True)
        result = subprocess.run(
            ["security", "find-generic-password", "-D", "AirPort network password",
             "-a", account, "-w"],
            capture_output=True, text=True, timeout=120,
        )
        if result.returncode == 0:
            password = result.stdout.removesuffix("\n")
            del result
            print("Connecting BOX-3 (SSID and credential not displayed)", flush=True)
            connect(args.ssid, password)
            break
        if result.returncode not in {44, 45}:
            raise SystemExit("Keychain access was denied or failed; no permission bypass attempted.")
    else:
        raise SystemExit("No matching saved Wi-Fi credential was found.")
except subprocess.TimeoutExpired:
    raise SystemExit("Timed out waiting for Keychain permission.")
except serial.SerialException:
    raise SystemExit("BOX-3 serial port is unavailable.")
except KeyboardInterrupt:
    raise SystemExit("Cancelled.")
