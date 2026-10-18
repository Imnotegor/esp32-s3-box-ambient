# CPU weather model for BOX

**Historical training → current wttr.in data → our CPU model → temperature/pressure charts on BOX → new observations and checked adaptation.**

The Rust/Candle model starts from random weights. Live weather inputs come only from wttr current conditions and their local archive. Provider future forecasts never enter the network, correct its output or act as a fallback. Open-Meteo is used for historical pretraining, not live inference.

## Model and data

Six weather features are temperature, pressure, humidity, two wind components and cloud cover. The model receives up to 73 hourly slots, calendar coordinates and an explicit presence mask. Training mixes 1, 6, 24 and 73 available points plus gaps. It can therefore start with one actual wttr response; missing hours are never presented as observations.

Three causal networks predict 72 future hourly temperature/pressure states; live output selects the next 48 future points. Training, adaptation and inference run on CPU. CUDA and Metal are disabled. `--threads` limits compute workers (default 2, range 1–32), overriding `RAYON_NUM_THREADS` before threads start. Network I/O uses one asynchronous runtime thread.

The initial ERA5 archive contains 94,440 hourly states from January 2016 through October 2026. Only the central location of the retained regional archive enters this model. Chronological training/tuning/calibration/holdout splits are 70/10/10/10%; targets crossing boundaries are purged. Normalization uses training data only, and intervals are calibrated separately for the four history sizes.

Holdout MAE in the 25–48-hour lead bin:

| Available history points | Temperature, °C | Pressure, hPa |
| --- | ---: | ---: |
| 1 | 2.97 | 5.32 |
| 6 | 2.89 | 5.21 |
| 24 | 2.83 | 5.18 |
| 73 | 2.83 | 5.21 |

These are retrospective ERA5 results, not measured accuracy on wttr or proof of superiority to operational forecasts. The BOX labels these outputs as model estimates. Nominal 90% bands are historical calibration estimates; their live coverage remains unverified. Scores from earlier regional/corrector experiments do not describe this model.

## Operating cycle

`serve` polls wttr every ten minutes, saves new source snapshots, runs the trained model and serves cached JSON for BOX. API reads never train a model or call the provider. There is no browser interface. Repeated current-condition payloads do not create new history or reset the original sample's age. Explicit source UTC datetimes are retained; when only an undated clock is available, the sample is dated by its **first receipt**, preserved in the data contract. No timezone is invented.

Updates fail on missing, invalid or stale input. BOX dims cached curves and hides expired forecasts. Tap the temperature/pressure cards to select the graph, and the 24H/48H button to select its horizon. The shaded band shows historical 90% calibration; pressure is displayed in mmHg. History contains actual saved wttr responses. New model/input combinations are archived before later source values arrive; internal diagnostics compare them with subsequently received values within 30 minutes, without inventing an accuracy score before matches exist.

`adapt` is a separate CPU job. It requires at least 90 days of genuine source history, uses a rolling 180-day window, excludes missing future targets from loss and purges targets crossing chronological partitions. It fine-tunes the existing own-model weights, chooses checkpoints on tuning data and recalibrates intervals on a separate block. A candidate replaces the active model only if holdout MAE improves by at least 2% for both temperature and pressure. Failed candidates are retained for inspection. At least seven additional days must accumulate before another adaptation attempt. This gate is empirical validation, not a guarantee of future improvement; polling never performs training implicitly.

## Run

From this directory, copy `config.example.json` outside Git, fill coordinates and the wttr location URL, and choose an absolute writable state directory.

```sh
cargo build --release --locked
./target/release/box3-weather-engine --config /path/to/config.json collect \
  --start 2016-01-01 --end 2026-10-09
./target/release/box3-weather-engine --config /path/to/config.json --threads 2 train --epochs 40
./target/release/box3-weather-engine --config /path/to/config.json serve \
  --bind 127.0.0.1:8790 --refresh-seconds 600
```

The listener is loopback-only. Configure the existing telemetry service with `BOX3_FORECAST_URL=http://127.0.0.1:8790/v1/box-forecast`; it relays bounded forecast data through the existing mTLS dashboard to BOX. The root URL returns 404. The application does not alter firewalls, VPN settings or existing services. `systemd/box3-weather-engine.service` is a user-service template; install the native Linux binary and set the config path before enabling it.

```sh
./target/release/box3-weather-engine --config /path/to/config.json forecast
./target/release/box3-weather-engine --config /path/to/config.json report
./target/release/box3-weather-engine --config /path/to/config.json --threads 2 adapt --epochs 10
./target/release/box3-weather-engine --config /path/to/config.json --threads 2 benchmark \
  --input /path/to/state/wttr-input.json --iterations 2000
./target/release/box3-weather-engine --config /path/to/config.json import-wttr --path /path/to/response.json
```

Imported responses use `{"received_at": UNIX_SECONDS, "response": WTTR_JSON}` and must arrive chronologically. This imports real saved responses; it does not reconstruct unobserved history. Historical collection dates must leave at least seven days for publication. A normal refresh never requires retraining.

## Artifacts and checks

- `active-wttr-model.json`, `wttr-models/<generation>/`: version 3 weights, checksums, calibration, reports and optional adaptation decisions. Earlier generations remain intact.
- `wttr-input.json`, `wttr-forecast.json`, `wttr-report.json`: latest input, output and training evaluation.
- `wttr/<site-key>/observations/`, `raw/`, `forecasts/`: source observations, raw responses and issued forecasts, separated by configured site.
- `GET /v1/box-forecast`: 25 bounded fixed-point forecast samples for the telemetry relay and BOX.
- `GET /api/state`: internal JSON diagnostics, without an HTML interface.

Local Apple M2 measurements: median 0.323 ms, p95 0.336 ms for all three networks on two CPU threads over 2,000 iterations, excluding networking and file/model loading. This is not a deployment-host measurement. A 3 °C change to the current input changed the trained model's first prediction by approximately 3 °C; tests verify that future wttr fields are excluded.

```sh
cargo fmt --check
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
```

Tests cover CPU backpropagation, optimized output/gradient equivalence, future-data isolation, normalization and temporal boundaries, current-input mapping, missing history, source deduplication, subsequent-value scoring and adaptation target masks. The BOX implementation is verified using native LVGL snapshots, protocol checks and hardware USB delivery; deployment of the new relay on the target host still requires SSH access. The earlier `tools/compare_archived_nwp.py` utility concerns the retained regional v2 experiment, not the active wttr model.

Source contracts: [wttr JSON format](https://github.com/chubin/wttr.in#json-output), [Open-Meteo historical data](https://open-meteo.com/en/docs/historical-weather-api).
