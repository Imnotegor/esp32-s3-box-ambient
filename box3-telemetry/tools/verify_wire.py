"""Inspect only BOX-3 TCP443 for 15s; retain metadata, never raw packet bytes."""
import argparse
import json
import os
import re
import selectors
import shlex
import subprocess
import time
from pathlib import Path

import serial

REMOTE = r'''
import socket,struct,time,json
# ETH_P_ALL includes outgoing frames; ETH_P_IP alone observes only incoming here.
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(3))
s.bind((CONFIG['interface'],0));s.settimeout(.3)
print('CAPTURE_READY',flush=True)
count=0;payloads=[];versions=[];suites=[]
end=time.monotonic()+15
try:
 while time.monotonic()<end:
  try:f=s.recv(65535)
  except socket.timeout:continue
  if len(f)<54 or f[12:14]!=b'\x08\x00' or f[23]!=6:continue
  if CONFIG['device_ip'] not in [socket.inet_ntoa(f[26:30]),socket.inet_ntoa(f[30:34])]:continue
  tcp=14+(f[14]&15)*4
  if len(f)<tcp+20:continue
  source,dest=struct.unpack('!HH',f[tcp:tcp+4])
  if 443 not in [source,dest]:continue
  count+=1
  p=f[tcp+(f[tcp+12]>>4)*4:];payloads.append(p)
  if source==443 and len(p)>49 and p[0]==22 and p[5]==2:
   i=44+p[43];suites.append(p[i:i+2].hex());i+=3
   stop=i+2+int.from_bytes(p[i:i+2],'big');i+=2
   while i+4<=min(stop,len(p)):
    kind=int.from_bytes(p[i:i+2],'big');size=int.from_bytes(p[i+2:i+4],'big');i+=4
    if kind==43:versions.append(p[i:i+size].hex())
    i+=size
finally:s.close()
raw=b''.join(payloads)
print(json.dumps({'captured_tcp443_frames':count,'tls_server_supported_versions':versions,
 'tls_server_ciphers':suites,'application_record_frames':sum(bool(p) and p[0]==23 for p in payloads),
 'plaintext_telemetry_fields_found':any(x in raw for x in [b'temperature_mc',b'humidity_milli_percent',b'PRIVATE KEY']),
 'raw_packets_saved':False}))
'''

parser = argparse.ArgumentParser()
parser.add_argument("--restart-receiver", action="store_true",
                    help="Briefly restart only the telemetry service to observe a fresh TLS handshake")
args = parser.parse_args()
config = {"interface": os.environ["BOX3_CAPTURE_INTERFACE"], "device_ip": os.environ["BOX3_DEVICE_IP"]}
REMOTE = "CONFIG = " + repr(config) + "\n" + REMOTE
ssh_host = os.environ["BOX3_SSH_HOST"]

port = serial.Serial()
port.port = os.environ["BOX3_SERIAL_PORT"]
port.baudrate = 115200
port.timeout = .1
port.dtr = False
port.rts = False
port.open()
with port:
    end = time.monotonic() + 20
    while time.monotonic() < end:
        port.read(4096)
    port.write(b"\x03\r")
    time.sleep(.3)
    port.reset_input_buffer()
    process = subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", ssh_host, "sudo -n python3 -c " + shlex.quote(REMOTE)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(8), "Capture did not start"
        assert process.stdout.readline().strip() == "CAPTURE_READY"
        if args.restart_receiver:
            subprocess.run(["ssh", "-o", "BatchMode=yes", ssh_host,
                            "sudo -n systemctl restart box3-telemetry"], check=True, timeout=12)
        port.write(b"box3_send\r")
        port.flush()
        output, error = process.communicate(timeout=22)
        assert process.returncode == 0, error
        result = json.loads(output.strip())
        path = Path(__file__).resolve().parents[1] / "wire-verification.json"
        path.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        if result["tls_server_supported_versions"] or args.restart_receiver:
            assert "0304" in result["tls_server_supported_versions"], "TLS1.3 ServerHello not observed"
            assert "1301" in result["tls_server_ciphers"], "Expected AES128-GCM-SHA256 not observed"
        else:
            print("Existing keep-alive session: negotiation not re-observed; use --restart-receiver for full check")
        assert result["application_record_frames"] > 0
        assert not result["plaintext_telemetry_fields_found"]
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
