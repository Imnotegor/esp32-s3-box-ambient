# ESP32-S3-BOX ambient dashboard

A private Wi-Fi dashboard for ESP32-S3-BOX-3: room sensors, Linux server metrics and CPU-generated weather forecasts.

The touchscreen provides three pages:

- **Home**: local AHT30 temperature and humidity, server/weather summary.
- **Server**: CPU history, RAM/SWAP, load and uptime from Linux `/proc`.
- **Weather**: temperature/pressure forecast charts, 24H/48H selection and historically calibrated uncertainty bands.

The Rust service samples server metrics every 2 seconds. The BOX polls every 5 seconds over reused TLS 1.3; sensor uploads remain hourly. A Docker-hosted CPU model combines trained weights with current wttr observations and saved history to refresh a 48-hour forecast every 10 minutes. It does not consume provider future forecasts or retrain on each update.

The model's API is loopback-only; forecasts reach the BOX through the existing mTLS service. Persistent model state and automatic container restarts keep operation independent of a laptop. Cached values retain their age when a source is unavailable.

See [`TODO.md`](TODO.md) for the model-status UI roadmap and acceptance checks.

## Repository layout

| Directory | Purpose |
| --- | --- |
| `box3-zephyr/` | Current C/Zephyr firmware, LVGL interface, provisioning and diagnostic tools |
| `box3-telemetry/` | Rust HTTPS service, forecast relay, tmux client and private-config tools |
| `weather-engine/` | CPU weather model, observation archive and Docker Compose deployment |
| `firmware/box3-app/` | Earlier Rust/ESP-IDF speaker smoke test, preserved separately |
| `firmware/components/` | Bridge components for that earlier smoke test |

The root Cargo workspace still describes the original ESP-IDF experiment. The host-side `box3-telemetry` and `weather-engine` crates are excluded and built independently. To check telemetry:

```sh
cd box3-telemetry
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s client
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools -p 'test_*.py'
```

See [`box3-zephyr/README.md`](box3-zephyr/README.md) for firmware prerequisites, [`box3-telemetry/README.md`](box3-telemetry/README.md) for mTLS setup, and [`weather-engine/README.md`](weather-engine/README.md#continuous-operation-in-docker) for Docker deployment, persistent state and model limitations.

## Security and local configuration

This repository contains source only: no operational keys, certificates, Wi-Fi credentials, private deployment settings, flash dumps, binaries or diagnostic archives. Create an owner-only private configuration using `box3-telemetry/development.example.json`; keep the populated file outside the checkout.

mTLS authenticates the device and dashboard separately. CA, certificate-date and peer identity verification remain mandatory. The device verifies the provisioned IP against the certificate IP SAN; there is no plaintext fallback. Physical extraction of unencrypted NVS is outside the network-security protection.

`SOURCE_SHA256SUMS` records this revision's source files, excluding the checksum manifest itself. The original bring-up results are in [`box3-telemetry/RESULTS.md`](box3-telemetry/RESULTS.md); model evaluation and deployment checks are described in [`weather-engine/README.md`](weather-engine/README.md). A successful build or API test is not proof of the physical display's appearance.

Only share audited source. Local SDKs, build outputs, private settings and old archives are not publication artifacts. Removing a credential from code does not revoke it if it was previously exposed.
