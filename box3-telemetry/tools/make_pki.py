"""Generate private local PKI; no keys are written into the source tree."""
import os
import ipaddress
import re
import shutil
import subprocess
from pathlib import Path

os.umask(0o077)
server_name = os.environ["BOX3_SERVER_NAME"]
dns_names = os.environ["BOX3_SERVER_DNS"].split(",")
server_ips = os.environ["BOX3_SERVER_IPS"].split(",")
if not all(re.fullmatch(r"[A-Za-z0-9.-]{1,253}", name) for name in [server_name, *dns_names]):
    raise SystemExit("Invalid server DNS identity")
for address in server_ips:
    ipaddress.ip_address(address)
openssl_binary = os.environ.get("BOX3_OPENSSL") or shutil.which("openssl")
if not openssl_binary:
    raise SystemExit("OpenSSL is required")
root = Path.home() / ".local/share/box3-telemetry/pki"
if root.exists():
    raise SystemExit("PKI already exists; refusing to replace identities")
root.mkdir(parents=True, mode=0o700)


def openssl(*args):
    subprocess.run([openssl_binary, *args], cwd=root,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def key(name):
    openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256",
            "-out", name + ".key")


for name in ["device-ca", "reader-ca"]:
    key(name)
    openssl("req", "-new", "-x509", "-key", name + ".key", "-sha256", "-days", "3650",
            "-subj", "/CN=BOX3-" + name, "-out", name + ".pem",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

for name, issuer, purpose in [
    ("server", "device-ca", "serverAuth"),
    ("box3", "device-ca", "clientAuth"),
    ("tmux", "reader-ca", "clientAuth"),
]:
    key(name)
    openssl("req", "-new", "-key", name + ".key", "-subj",
            "/CN=" + (server_name if name == "server" else name),
            "-out", name + ".csr")
    ext = "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
    ext += "extendedKeyUsage=" + purpose + "\n"
    if name == "server":
        ext += "subjectAltName=" + ",".join(["DNS:" + n for n in dns_names] + ["IP:" + a for a in server_ips]) + "\n"
    (root / (name + ".ext")).write_text(ext)
    openssl("x509", "-req", "-in", name + ".csr", "-CA", issuer + ".pem",
            "-CAkey", issuer + ".key", "-CAcreateserial", "-sha256", "-days", "397",
            "-extfile", name + ".ext", "-out", name + ".pem")
print("Private PKI created:", root)
print("CA private keys remain local; do not put this directory in archives.")
