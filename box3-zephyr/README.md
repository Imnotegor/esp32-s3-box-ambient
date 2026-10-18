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

The GUI uses a 384KiB PSRAM pool and a 12KiB guarded main stack. All LVGL operations execute on the main thread. Sensor uploads are hourly; dashboard polling is every 5 seconds. Bluetooth is disabled.

## Weather forecast on BOX

WEATHER contains temperature and pressure cards and one large forecast plot. Tap either card to choose the plotted variable; tap 24H/48H to change the horizon. The shaded area is the model's historically calibrated 90% interval, not a guarantee of live accuracy. Pressure is displayed in mmHg. Forecast computation and training remain on the host CPU; BOX renders at most 25 bounded integer points. Existing page transitions are retained.

The existing authenticated `/v1/dashboard` response can include an optional `forecast` object. Older server responses remain accepted. The HTTP body limit is 4096 bytes; malformed, missing or out-of-range point fields are rejected. Cached data retain their original age, dim after 15 minutes without a new forecast, and disappear after two hours or when source data exceed six hours. The forecast's zero-hour axis is relative to its issue time.

For trusted USB validation while host deployment is unavailable, `tools/send_forecast.py` reads the local CPU engine's `/v1/box-forecast` endpoint. It sends bounded chunks, verifies all plotted values with a checksum and labels the display `USB MODEL`. It does not write NVS or alter TLS configuration. Use the private-settings runner, the existing Python serial environment and `--exercise` for malformed-packet/control checks. `--watch` updates every ten minutes while USB remains connected, keeps one console connection open, and preserves the user's selected page and graph.

The native preview accepts an optional text fixture: four integers (forecast age, source age, current centidegrees C, current deci-hPa), followed by 25 rows of `m t p lo hi pl ph`. `m` is minutes from issue, `t/lo/hi` are centidegrees C and `p/pl/ph` are deci-hPa. Generated fixtures remain design tests; actual forecast fixtures are distinguished in their diagnostics.

## Page transitions

Pages enter with a short 16-pixel horizontal shift and a subtle 90–100% opacity change over 160 ms, with quick response and gentle deceleration. The header and navigation stay fixed; the tab indicator follows the same easing curve. One reusable RGB565 snapshot in PSRAM avoids redrawing text and charts on every animation frame; live widgets resume after the transition. During a transition, only the latest additional page request is retained. Selecting the current page does not repaint it. If a snapshot cannot be produced, navigation switches immediately.

The display uses the BOX-3 BSP's 40 MHz clock, SPI DMA and two full-screen RGB565 buffers (300 KiB total in PSRAM). Partial refresh remains enabled, so unchanged areas are not transmitted. Each dirty region is rendered completely before a separate flush thread sends it, avoiding render pauses between horizontal strips. Refresh scheduling is 16 ms; actual frame time depends on rendering and SPI transfer, so this is not a 60 FPS guarantee. The AHT30's conversion wait is scheduled without blocking the UI loop.

USB diagnostics report `BOX3_UI settled` with transition time, and `BOX3_UI render` with render/flush-submission timing. The latter excludes any final DMA transfer still in flight. `box3_page 0`, `1`, or `2` exercises navigation without touching the screen.

The host preview checks partial-flush output against complete snapshots, rapid navigation, repeated-transition memory use and redundant redraws. With `ZEPHYR_WORKSPACE` pointing to the pinned workspace:

```sh
cmake -S box3-telemetry/preview -B /tmp/box3-preview \
  -DLVGL_DIR="$ZEPHYR_WORKSPACE/modules/lib/gui/lvgl"
cmake --build /tmp/box3-preview -j4
mkdir -p box3-telemetry/artifacts/dashboard-preview
/tmp/box3-preview/preview box3-telemetry/artifacts/dashboard-preview
python3 box3-telemetry/preview/to_png.py
```

Run these commands from the repository root. Preview values are synthetic; device timing and physical display quality require a hardware check.

The hardware validation run completed 24 page transitions in 196–276 ms including snapshot preparation (median 248 ms), with 25 CRC-valid sensor samples, ten successful dashboard polls and a successful telemetry upload. No UI assertion, stack overflow or allocation failure was recorded; GUI heap usage remained at 36%. The native preview also passed 30 rapid-navigation sequences and an update-during-transition check. These are timing and correctness checks, not a claim of visually verified tear-free or 60 FPS output.

A build is not an instruction to flash. Flashing/deployment requires a separate operational decision. Publish source-only exports, not SDKs, build folders, logs or existing firmware dumps.
