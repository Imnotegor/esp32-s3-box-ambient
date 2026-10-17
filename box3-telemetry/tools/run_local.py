"""Run a local tool with private deployment settings; never print their values."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys


def load_settings(path):
    with Path(path).open() as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("Private settings must be owned by this user with mode 0600")
        values = json.load(stream)
    if not isinstance(values, dict) or not all(
        isinstance(k, str) and k.startswith("BOX3_") and k.replace("_", "").isalnum()
        and isinstance(v, str) and "\0" not in v
        for k, v in values.items()
    ):
        raise ValueError("Expected a JSON object of BOX3_* string settings")
    return values


def main():
    if len(sys.argv) < 2:
        raise SystemExit("Usage: python tools/run_local.py COMMAND [ARGS...]")
    path = os.environ.get("BOX3_LOCAL_CONFIG", str(Path.home() / ".config/box3-telemetry/development.json"))
    try:
        values = load_settings(path)
    except (OSError, ValueError):
        raise SystemExit("Private settings unavailable, invalid or not mode 0600") from None
    # Explicit environment overrides win. Values never enter command-line arguments.
    environment = os.environ.copy()
    for key, value in values.items():
        environment.setdefault(key, value)
    return subprocess.run(sys.argv[1:], env=environment).returncode


if __name__ == "__main__":
    sys.exit(main())
