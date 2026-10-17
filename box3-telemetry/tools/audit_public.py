"""Bounded public-source audit. Reports locations/categories, never matched values."""
import argparse
import ipaddress
import json
from pathlib import Path
import re

PATTERNS = {
    "private-key-material": re.compile(rb"-----BEGIN (?:EC |RSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----"),
    "cloud-access-key": re.compile(rb"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b"),
    "provider-token": re.compile(rb"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|xox[baprs]-[A-Za-z0-9-]{20,})\b"),
    "literal-credential": re.compile(rb"(?i)\b(?:password|psk|token|secret|api_key)\s*=\s*[\"'][^\"'\r\n]{8,}[\"']"),
    "personal-home-path": re.compile(rb"/(?:Users|home)/[A-Za-z0-9._-]+"),
    "device-mac": re.compile(rb"\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b"),
}
DOCUMENTATION_NETWORKS = tuple(ipaddress.ip_network(n) for n in ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24"))
SKIP = {".git", ".venv", "workspace", "sdk", "target", "__pycache__", "artifacts"}
FORBIDDEN_SUFFIXES = {".pem", ".key", ".csr", ".srl", ".pcap", ".bin", ".elf"}


def findings(data, known=()):
    results = {kind for kind, pattern in PATTERNS.items() if pattern.search(data)}
    lower = data.lower()
    if any(value.lower() in lower for value in known if len(value) >= 6):
        results.add("configured-private-value")
    for value in re.findall(rb"\b(?:\d{1,3}\.){3}\d{1,3}\b", data):
        try:
            address = ipaddress.ip_address(value.decode())
        except ValueError:
            continue
        if (address.is_private and not address.is_loopback and not address.is_unspecified
                and not any(address in net for net in DOCUMENTATION_NETWORKS)):
            results.add("private-network-address")
    return sorted(results)


def audit(root, known=(), strict=False):
    results = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if not strict and (any(part in SKIP for part in relative.parts) or any(part.startswith("build") for part in relative.parts[:-1])):
            continue
        if path.is_symlink():
            results.append((str(relative), ["symlink-not-public-source"]))
        elif path.is_file():
            kinds = findings(path.read_bytes(), known)
            if path.suffix in FORBIDDEN_SUFFIXES or path.name in {".env", "development.json"}:
                kinds.append("private-or-binary-artifact")
            if kinds:
                results.append((str(relative), sorted(set(kinds))))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--private-settings", type=Path)
    parser.add_argument("--strict", action="store_true", help="Scan every export entry without vendor/build exclusions")
    args = parser.parse_args()
    if not args.root.is_dir():
        parser.error("Audit root must be an existing directory")
    known = []
    if args.private_settings:
        from run_local import load_settings
        for value in load_settings(args.private_settings).values():
            known.extend(part.encode() for part in value.split(",") if part)
    found = audit(args.root, known, strict=args.strict)
    for path, categories in found:
        print(json.dumps({"file": path, "categories": categories}))
    print("Public-source audit:", "FAIL" if found else "PASS")
    return bool(found)


if __name__ == "__main__":
    raise SystemExit(main())
