"""Install/reproduce the Mac cache updater and rootless private-network proxy."""
import json
import os
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path

os.umask(0o077)
home = Path.home()
project = Path(__file__).resolve().parents[1]
private = home / ".local/share/box3-telemetry"
pki = private / "pki"
for name in ["device-ca.pem", "tmux.pem", "tmux.key"]:
    if not (pki / name).is_file():
        raise SystemExit("Install your private reader identity first; missing " + name)
tailscaled = shutil.which("tailscaled")
if not tailscaled:
    raise SystemExit("Install the tailscale Homebrew formula first")
config = home / ".config/box3-telemetry"
cache = home / ".cache/box3-telemetry"
state = private / "tailnet"
for path in [private, config, cache, state]:
    path.mkdir(parents=True, exist_ok=True)
    path.chmod(0o700)
client = home / ".tmux/box3-status.py"
client.parent.mkdir(exist_ok=True)
shutil.copyfile(project / "client/status.py", client)
client.chmod(0o700)
settings = {"url": os.environ["BOX3_READER_URL"], "proxy": "127.0.0.1:10555",
            "ca": str(pki / "device-ca.pem"), "cert": str(pki / "tmux.pem"),
            "key": str(pki / "tmux.key"), "cache": str(cache / "status.json")}
fd, temporary = tempfile.mkstemp(dir=str(config))
with os.fdopen(fd, "w") as f:
    json.dump(settings, f, indent=2)
    f.write("\n")
os.replace(temporary, config / "client.json")

agents = [
    {"Label": "local.box3-tailnet", "ProgramArguments": [tailscaled,
     "--tun=userspace-networking", "--socks5-server=127.0.0.1:10555",
     "--socket=" + str(state / "tailscaled.sock"), "--state=" + str(state / "tailscaled.state"),
     "--no-logs-no-support"], "RunAtLoad": True, "KeepAlive": True,
     "StandardOutPath": str(state / "daemon.log"), "StandardErrorPath": str(state / "daemon-error.log"),
     "Umask": 0o077},
    {"Label": "local.box3-status", "ProgramArguments": ["/usr/bin/python3", str(client), "refresh"],
     "RunAtLoad": True, "StartInterval": 60, "StandardOutPath": str(cache / "updater.log"),
     "StandardErrorPath": str(cache / "updater-error.log"), "Umask": 0o077},
]
for agent in agents:
    path = home / "Library/LaunchAgents" / (agent["Label"] + ".plist")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and plistlib.loads(path.read_bytes()) != agent:
        raise SystemExit("Existing LaunchAgent differs; inspect before replacing " + str(path))
    path.write_bytes(plistlib.dumps(agent))
    path.chmod(0o600)
    loaded = subprocess.run(["launchctl", "print", "gui/{}/{}".format(os.getuid(), agent["Label"])],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if loaded.returncode:
        subprocess.run(["launchctl", "bootstrap", "gui/{}".format(os.getuid()), str(path)], check=True)
print("Client and managed services installed; existing VPN login and global routes preserved")
print("tmux rendering command: /usr/bin/python3 ~/.tmux/box3-status.py render")
