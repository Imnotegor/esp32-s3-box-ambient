# ESP32-S3-BOX ambient dashboard

A private Wi-Fi temperature/humidity and server dashboard for ESP32-S3-BOX-3.

The touchscreen provides three pages:

- **Home**: local AHT30 temperature and humidity, server/weather summary.
- **Server**: CPU history, RAM/SWAP, load and uptime from Linux `/proc`.
- **Weather**: configured outdoor temperature, feels-like, humidity and cache age.

The Rust service samples server metrics every 2 seconds. The BOX polls every 5 seconds over reused TLS 1.3; sensor uploads remain hourly. Weather is cached server-side, allowing the display and tmux client to use the same source without keeping a laptop connected.

## Repository layout

| Directory | Purpose |
| --- | --- |
| `box3-zephyr/` | Current C/Zephyr firmware, LVGL interface, provisioning and diagnostic tools |
| `box3-telemetry/` | Rust HTTPS service, tmux client, private-config tools and source audit |
| `firmware/box3-app/` | Earlier Rust/ESP-IDF speaker smoke test, preserved separately |
| `firmware/components/` | Bridge components for that earlier smoke test |

The root Cargo workspace still describes the original ESP-IDF experiment. The host-side `box3-telemetry` crate is deliberately excluded and built independently:

```sh
cd box3-telemetry
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s client
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools -p 'test_*.py'
```

See [`box3-zephyr/README.md`](box3-zephyr/README.md) for firmware build prerequisites and pinned dependencies. See [`box3-telemetry/README.md`](box3-telemetry/README.md) for setup, trust separation, private settings and deployment.

## Security and local configuration

This repository contains source only: no operational keys, certificates, Wi-Fi credentials, private deployment settings, flash dumps, binaries or diagnostic archives. Create an owner-only private configuration using `box3-telemetry/development.example.json`; keep the populated file outside the checkout.

mTLS authenticates the device and dashboard separately. CA, certificate-date and peer identity verification remain mandatory. The device verifies the provisioned IP against the certificate IP SAN; there is no plaintext fallback. Physical extraction of unencrypted NVS is outside the network-security protection.

`SOURCE_SHA256SUMS` records the imported sanitized source snapshot. Tests, compilation and source-audit results are documented in [`box3-telemetry/RESULTS.md`](box3-telemetry/RESULTS.md). The source-sanitization revision was compiled but has **not** yet been deployed/flashed onto the running installation; do not confuse build evidence with a hardware runtime test of that revision.

Only share audited source. Local SDKs, build outputs, private settings and old archives are not publication artifacts. Removing a credential from code does not revoke it if it was previously exposed.
