"""Local hidden credential input; serial replies are filtered, never dumped or saved."""
import getpass
import ipaddress
import json
import os
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path.home() / ".local/state/box3-diagnostics"
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def shell_quote(value):
    parts = []
    for byte in value.encode("utf-8"):
        if byte == 34:
            parts.append('\\"')
        elif byte == 92 or byte < 32 or byte > 126:
            parts.append(f"\\x{byte:02x}")
        else:
            parts.append(chr(byte))
    return '"' + "".join(parts) + '"'


def receive(port, seconds):
    data = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        data.extend(port.read(2048))
    return ANSI.sub("", data.decode(errors="replace")).replace("uart:~$ ", "")


def command(port, text, seconds=2):
    port.write((text + "\r").encode("ascii"))
    port.flush()
    return receive(port, seconds)


def connect(ssid, password, mode="1"):
    if not 1 <= len(ssid.encode("utf-8")) <= 32 or any(ord(c) < 32 for c in ssid):
        raise SystemExit("SSID must contain 1–32 UTF-8 bytes, without control characters")
    if mode not in {"1", "5"}:
        raise SystemExit("This helper supports WPA2/WPA3 Personal only")
    if not 8 <= len(password.encode("utf-8")) <= 63 or any(ord(c) < 32 for c in password):
        raise SystemExit("Expected an 8–63 byte Personal passphrase, without control characters")
    request = (f"wifi connect -s {shell_quote(ssid)} -p {shell_quote(password)} "
               f"-k {mode} -b 2 -t 30")
    if len(request.encode("ascii")) >= 256:
        raise SystemExit("Escaped credentials exceed the firmware shell's 256-byte command buffer")

    with serial.Serial(os.environ["BOX3_SERIAL_PORT"], 115200, timeout=0.2) as port:
        command(port, "\x03", 0.5)
        command(port, "shell echo off", 0.5)
        try:
            command(port, request, 2)
            del request, password, ssid
            print("Connection requested; waiting for association and DHCP...", flush=True)
            for _ in range(10):
                status = command(port, "wifi status", 2)
                states = re.findall(r"State:\s*([A-Z_]+)", status)
                state = states[-1] if states else "UNKNOWN"
                addresses = command(port, "net ipv4", 2)
                ips = []
                for address in re.findall(r"(\d+\.\d+\.\d+\.\d+)/\d+\.\d+\.\d+\.\d+", addresses):
                    ip = ipaddress.IPv4Address(address)
                    if not ip.is_unspecified and not ip.is_multicast and not ip.is_loopback:
                        ips.append(str(ip))
                if state in {"COMPLETED", "ASSOCIATED"} and ips:
                    result = {"connected": True, "state": state, "ipv4": sorted(set(ips))}
                    os.umask(0o077)
                    ROOT.mkdir(parents=True, exist_ok=True, mode=0o700)
                    ROOT.chmod(0o700)
                    (ROOT / "wifi-result.json").write_text(json.dumps(result, indent=2) + "\n")
                    print("CONNECTED; private address saved outside source tree", flush=True)
                    return
                print("Wi-Fi state:", state, "| waiting for IPv4", flush=True)
            print("Connection/IP not confirmed. Check credentials, security mode and 2.4 GHz availability.")
            raise SystemExit(1)
        finally:
            command(port, "shell echo on", 0.5)


def main():
    if not os.isatty(sys.stdin.fileno()):
        raise SystemExit("A local terminal is required for hidden password entry")
    print("BOX-3 Wi-Fi (2.4 GHz). Credentials stay in RAM, not in files.")
    print("After a reset you may need to connect again.")
    ssid = getpass.getpass("Network name / SSID (hidden): ")
    mode = input("Security: 1=WPA2/transition, 5=WPA3-only [1]: ").strip() or "1"
    password = getpass.getpass("Wi-Fi password (hidden): ")
    connect(ssid, password, mode)


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        print("\nCancelled; no credentials saved.")
        raise SystemExit(1)
    except serial.SerialException:
        raise SystemExit("Serial port unavailable. Reconnect the BOX-3 and close other serial tools.")
