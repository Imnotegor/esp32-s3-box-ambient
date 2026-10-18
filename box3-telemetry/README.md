# BOX-3 private dashboard

ESP32-S3-BOX-3 → mTLS 1.3 Rust service → private VPN → cached tmux client.

## Source layout

- `src/`: Linux CPU/RAM/SWAP/load/uptime collection, cached weather, authenticated telemetry receiver.
- `client/`: cache updater and network-free tmux renderer.
- `tools/`: provisioning/deployment support, private configuration runner and public-source audit.
- `systemd/`: portable service template.
- `preview/`: native LVGL renderer using synthetic fixtures, not live-device evidence.
- Firmware is maintained separately in the sibling `box3-zephyr` source directory.

Three touchscreen pages show local temperature/RH, server metrics and outdoor weather. Server metrics are sampled every 2 seconds; the device polls every 5 seconds over reused TLS. Sensor uploads remain hourly. Missing/stale values are marked rather than fabricated.

The optional CPU forecast relay is enabled with `BOX3_FORECAST_URL=http://127.0.0.1:8790/v1/box-forecast`. Only a literal loopback HTTP endpoint is accepted, and redirects are disabled. The neural service remains headless; all forecast charts are on BOX. A bounded `forecast` object is added to the existing mTLS dashboard, with temperature/pressure intervals and 25 fixed-point samples. Failed refreshes do not reset cached ages. Install the forecast-capable firmware before enabling this relay because its authenticated dashboard response can exceed the old firmware's 1024-byte buffer.

## Private deployment configuration

There are no personal network, device or account defaults in the portable source.
Copy `development.example.json` to `~/.config/box3-telemetry/development.json`, fill it locally and set permissions to `0600`. The file must remain outside all source bundles. Existing installations may already have this private file.

Run tools with settings provided only in their environment:

```sh
python3 tools/run_local.py python3 tools/install_client.py
python3 tools/run_local.py python3 tools/deploy.py
python3 tools/run_local.py python3 ../box3-zephyr/tools/flash_capture.py build-dashboard test-run
```

Explicit environment settings override the file. `BOX3_LOCAL_CONFIG` can select another private configuration file. The runner does not print configuration values or turn them into command arguments. Wi-Fi passwords are still read with hidden terminal input, not saved in this file. PKI generation refuses to replace existing identities; never rerun it as a routine update.

`BOX3_WEATHER_URL` selects the HTTPS weather endpoint. The server adds the required metric/plain-text format query parameters. Weather endpoint failures are logged without URLs or credentials. The device uses generic SERVER/OUTDOOR labels; the city is not compiled into the UI.

## Service and trust

The service template reads `/etc/box3-telemetry/runtime.env`. Deployment generates this root-only file from private settings. It contains `BOX3_INGEST_ADDR`, `BOX3_READ_ADDR` and `BOX3_WEATHER_URL`. Existing PKI/state paths are application-owned standard paths, not personal defaults.

The deploy helper does **not** edit firewall rules. The administrator must permit HTTPS only on the intended LAN/VPN interfaces before deploying. Do not replace private bind addresses with public wildcard listeners casually.

Separate client CAs restrict device and dashboard roles. The device can post measurements and read only its server/weather dashboard; the reader identity accesses stored measurements over the VPN listener. No unauthenticated endpoint, disabled certificate verification or plaintext fallback is provided.

The firmware verifies the server's **provisioned IP address against the certificate IP SAN**. The PKI generator obtains DNS/IP SANs from private settings. CA trust, certificate dates, required peer verification and TLS 1.3 remain enforced. NVS settings are not compiled into application images.

## Audit and publication

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/audit_public.py SOURCE_DIRECTORY --strict \
  --private-settings "$HOME/.config/box3-telemetry/development.json"
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools -p 'test_*.py'
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
```

The bounded audit checks private addresses, personal home paths, MACs, known private configuration values, common token/key patterns and literal credential assignments. It prints categories/file locations, never matched values. It is a regression guard, not proof that every possible unknown secret has been identified.

Publish only the explicitly exported source bundle. Never publish working directories wholesale: SDK/build outputs, device dumps, logs, certificates, local settings, VPN state, caches and historical release archives may carry private information. Historical project artifacts are retained in the owner's private state directory, outside portable source.

Any real credential discovered in previously shared content must be revoked/rotated; deleting it from current source does not revoke it or erase external copies.
