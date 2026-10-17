# BOX-3 firmware

Zephyr C firmware for ESP32-S3-BOX-3: local AHT30 readings, HOME/SERVER/WEATHER LVGL pages and TLS 1.3 telemetry.

Wi-Fi SSID/passphrase, server IP and client credentials are provisioned over trusted USB into NVS. They are not application-image defaults. Keep private configuration and flash/NVS dumps outside the source tree. NVS is not hardware-encrypted; physical-flash extraction remains outside this protection.

The server's provisioned IP is verified against its certificate IP SAN. Peer verification and certificate dates remain required; there is no TLS downgrade or plaintext fallback. No NVS schema migration is introduced by the source sanitization.

USB tools read BOX3_SERIAL_PORT and, for flashing, BOX3_DEVICE_MAC from the environment; MAC identity checking is mandatory. Provisioning also requires BOX3_WIFI_SSID and BOX3_SERVER_IP or explicit arguments, plus hidden password entry. Use the sibling telemetry project's tools/run_local.py to load local private settings. These settings never belong in source bundles.

New diagnostic logs go to ~/.local/state/box3-diagnostics with a private umask. Treat serial output as private. Historical logs, flash dumps, releases and rollback artifacts are in the owner's private project state directory.

## Build

Use the pinned dependencies in source-lock.json, a matching Zephyr SDK and a Python west environment. From the workspace directory:

```sh
west build -b esp32s3_box3/esp32s3/procpu ../app -d ../build-dashboard -o=-j4
```

The GUI uses a 128KiB PSRAM pool and a 12KiB guarded main stack. All LVGL operations execute on the main thread. Sensor uploads are hourly; dashboard polling is every 5 seconds. Bluetooth is disabled.

A build is not an instruction to flash. Flashing/deployment requires a separate operational decision. Publish source-only exports, not SDKs, build folders, logs or existing firmware dumps.
