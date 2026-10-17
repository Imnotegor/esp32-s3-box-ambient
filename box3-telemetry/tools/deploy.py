"""Deploy owned binary/config and only the server's key via SSH stdin."""
import base64
import json
import os
from urllib.parse import urlsplit
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
pki = Path.home() / ".local/share/box3-telemetry/pki"
files = {name: base64.b64encode((pki / name).read_bytes()).decode()
         for name in ["server.pem", "server.key", "device-ca.pem", "reader-ca.pem"]}
runtime = {key: os.environ[key] for key in ["BOX3_INGEST_ADDR", "BOX3_READ_ADDR", "BOX3_WEATHER_URL"]}
if any(any(c.isspace() for c in value) for value in runtime.values()) or urlsplit(runtime["BOX3_WEATHER_URL"]).scheme != "https":
    raise SystemExit("Invalid private service settings")
files["environment"] = base64.b64encode("".join(k + "=" + json.dumps(v) + "\n" for k, v in runtime.items()).encode()).decode()
files["binary"] = os.environ["BOX3_REMOTE_BINARY"]
files["unit"] = base64.b64encode((root / "systemd/box3-telemetry.service").read_bytes()).decode()

remote = r"""
import base64,json,sys,os,pwd,subprocess,pathlib
d=json.load(sys.stdin)
try: account=pwd.getpwnam('box3-telemetry')
except KeyError:
 subprocess.run(['/usr/sbin/useradd','--system','--user-group','--home-dir',
  '/var/lib/box3-telemetry','--shell','/usr/sbin/nologin','box3-telemetry'],check=True)
 account=pwd.getpwnam('box3-telemetry')
p=pathlib.Path('/etc/box3-telemetry/pki')
p.mkdir(parents=True,exist_ok=True)
for directory in [p,p.parent]:
 os.chown(directory,0,account.pw_gid);directory.chmod(0o750)
for name in ['server.pem','server.key','device-ca.pem','reader-ca.pem']:
 f=p/name
 f.write_bytes(base64.b64decode(d[name]))
 os.chown(f,0,account.pw_gid);f.chmod(0o640)
environment=p.parent/'runtime.env'
environment.write_bytes(base64.b64decode(d['environment']));environment.chmod(0o600)
unit=pathlib.Path('/etc/systemd/system/box3-telemetry.service')
unit.write_bytes(base64.b64decode(d['unit']));unit.chmod(0o644)
subprocess.run(['/usr/bin/install','-m','0755',
 d['binary'],
 '/usr/local/bin/box3-telemetry'],check=True)
subprocess.run(['systemd-analyze','verify',str(unit)],check=True)
# Firewall policy belongs to the local administrator, not to a portable deploy tool.
subprocess.run(['systemctl','daemon-reload'],check=True)
subprocess.run(['systemctl','enable','--now','box3-telemetry'],check=True)
subprocess.run(['systemctl','is-active','box3-telemetry'],check=True)
print('Rust HTTPS service deployed; CA signing keys were not uploaded')
"""
command = ["ssh", "-o", "BatchMode=yes", os.environ["BOX3_SSH_HOST"], "sudo -n python3 -c " +
           __import__("shlex").quote(remote)]
subprocess.run(command, input=json.dumps(files), text=True, check=True)
