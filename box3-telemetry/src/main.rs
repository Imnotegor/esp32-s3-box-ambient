mod dashboard;
mod forecast;

use anyhow::{Context, Result};
use axum::{
    extract::{DefaultBodyLimit, State},
    http::{header, StatusCode},
    response::IntoResponse,
    routing::{get, post},
    Json, Router,
};
use rustls::{RootCertStore, ServerConfig};
use serde::{Deserialize, Serialize};
use std::{
    env, fs,
    io::{BufReader, Write},
    net::SocketAddr,
    path::{Path, PathBuf},
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tokio::sync::RwLock;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Measurement {
    node_id: String,
    boot_id: String,
    sequence: u64,
    temperature_mc: i32,
    humidity_milli_percent: u32,
    sample_age_ms: u64,
    uptime_ms: u64,
    interval_secs: u64,
}

impl Measurement {
    fn validate(&self) -> bool {
        self.node_id == "box3"
            && self.boot_id.len() == 16
            && self.boot_id.bytes().all(|b| b.is_ascii_hexdigit())
            && self.sequence > 0
            && (-40_000..=85_000).contains(&self.temperature_mc)
            && self.humidity_milli_percent <= 100_000
            && self.sample_age_ms <= 15_000
            && self.uptime_ms >= self.sample_age_ms
            && (60..=86_400).contains(&self.interval_secs)
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct Snapshot {
    received_at_unix: u64,
    measurement: Measurement,
}

#[derive(Serialize)]
struct Latest {
    received_at_unix: u64,
    age_secs: u64,
    stale: bool,
    measurement: Measurement,
    dashboard: dashboard::DeviceDashboard,
}

struct Store {
    path: PathBuf,
    latest: RwLock<Option<Snapshot>>,
    dashboard: dashboard::Shared,
}

fn unix_now() -> Result<u64> {
    Ok(SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs())
}

fn stale(age: u64, interval: u64) -> bool {
    age > interval.saturating_mul(3) / 2 + 120
}

fn atomic_save<T: Serialize>(path: &Path, snapshot: &T) -> Result<()> {
    let parent = path.parent().context("state path has no parent")?;
    fs::create_dir_all(parent)?;
    let tmp = path.with_extension("tmp");
    let mut file = fs::File::create(&tmp)?;
    file.write_all(&serde_json::to_vec(snapshot)?)?;
    file.sync_all()?;
    fs::rename(&tmp, path)?;
    fs::File::open(parent)?.sync_all()?;
    Ok(())
}

async fn ingest(
    State(store): State<Arc<Store>>,
    Json(sample): Json<Measurement>,
) -> Result<StatusCode, StatusCode> {
    if !sample.validate() {
        return Err(StatusCode::UNPROCESSABLE_ENTITY);
    }
    let mut guard = store.latest.write().await;
    if let Some(previous) = guard.as_ref() {
        if previous.measurement.boot_id == sample.boot_id
            && sample.sequence <= previous.measurement.sequence
        {
            return Err(StatusCode::CONFLICT);
        }
    }
    let snapshot = Snapshot {
        received_at_unix: unix_now().map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?,
        measurement: sample,
    };
    let path = store.path.clone();
    let saved = snapshot.clone();
    tokio::task::spawn_blocking(move || atomic_save(&path, &saved))
        .await
        .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?
        .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    *guard = Some(snapshot);
    Ok(StatusCode::NO_CONTENT)
}

async fn latest(State(store): State<Arc<Store>>) -> Result<Json<Latest>, StatusCode> {
    let guard = store.latest.read().await;
    let snapshot = guard.as_ref().ok_or(StatusCode::NOT_FOUND)?;
    let now = unix_now().map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    let age = now.saturating_sub(snapshot.received_at_unix);
    Ok(Json(Latest {
        received_at_unix: snapshot.received_at_unix,
        age_secs: age,
        stale: stale(age, snapshot.measurement.interval_secs),
        measurement: snapshot.measurement.clone(),
        dashboard: dashboard::snapshot(&store.dashboard).await,
    }))
}

async fn device_dashboard(State(store): State<Arc<Store>>) -> impl IntoResponse {
    let data = dashboard::snapshot(&store.dashboard).await;
    match serde_json::to_vec(&data) {
        Ok(mut bytes) if bytes.len() <= 4096 => {
            let frame_size = if bytes.len() <= 1024 { 1024 } else { 4096 };
            bytes.resize(frame_size, b' ');
            (
                [
                    (header::CONTENT_TYPE, "application/json"),
                    (header::CACHE_CONTROL, "no-store"),
                ],
                bytes,
            )
                .into_response()
        }
        _ => StatusCode::INTERNAL_SERVER_ERROR.into_response(),
    }
}

fn tls(pki: &Path, client_ca: &str) -> Result<axum_server::tls_rustls::RustlsConfig> {
    let read_certs = |name: &str| -> Result<Vec<rustls::pki_types::CertificateDer<'static>>> {
        let mut reader = BufReader::new(fs::File::open(pki.join(name))?);
        Ok(rustls_pemfile::certs(&mut reader).collect::<std::io::Result<Vec<_>>>()?)
    };
    let mut roots = RootCertStore::empty();
    for cert in read_certs(client_ca)? {
        roots.add(cert)?;
    }
    let verifier = rustls::server::WebPkiClientVerifier::builder(Arc::new(roots)).build()?;
    let mut key_reader = BufReader::new(fs::File::open(pki.join("server.key"))?);
    let key =
        rustls_pemfile::private_key(&mut key_reader)?.context("missing server private key")?;
    let mut config = ServerConfig::builder()
        .with_client_cert_verifier(verifier)
        .with_single_cert(read_certs("server.pem")?, key)?;
    // HTTP headers plus a 4 KiB body exceed the BOX's 4 KiB TLS input limit.
    config.max_fragment_size = Some(2048);
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(axum_server::tls_rustls::RustlsConfig::from_config(
        Arc::new(config),
    ))
}

#[tokio::main]
async fn main() -> Result<()> {
    rustls::crypto::ring::default_provider()
        .install_default()
        .map_err(|_| anyhow::anyhow!("TLS crypto provider already installed"))?;
    let pki = PathBuf::from(env::var("BOX3_PKI_DIR")?);
    let path = PathBuf::from(env::var("BOX3_STATE_FILE")?);
    let lan: SocketAddr = env::var("BOX3_INGEST_ADDR")?.parse()?;
    let vpn: SocketAddr = env::var("BOX3_READ_ADDR")?.parse()?;
    let loaded = match fs::read(&path) {
        Ok(bytes) => {
            let snapshot: Snapshot = serde_json::from_slice(&bytes)?;
            anyhow::ensure!(snapshot.measurement.validate(), "invalid saved measurement");
            Some(snapshot)
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
        Err(error) => return Err(error.into()),
    };
    let dashboard = dashboard::start(
        path.with_file_name("weather.json"),
        env::var("BOX3_WEATHER_URL")?,
        env::var("BOX3_FORECAST_URL")
            .ok()
            .map(|url| forecast::validate_url(&url))
            .transpose()?,
    );
    let store = Arc::new(Store {
        path,
        latest: RwLock::new(loaded),
        dashboard,
    });
    // Different trust roots and listeners separate sensor writers from dashboard readers.
    let ingest_app = Router::new()
        .route("/v1/measurements", post(ingest))
        .route("/v1/dashboard", get(device_dashboard))
        .layer(DefaultBodyLimit::max(1024))
        .with_state(store.clone());
    let read_app = Router::new()
        .route("/v1/latest", get(latest))
        .with_state(store);
    let ingest_tls = tls(&pki, "device-ca.pem")?;
    let read_tls = tls(&pki, "reader-ca.pem")?;
    println!("mTLS ingest={lan}; mTLS read={vpn}");
    let mut ingest_server = axum_server::bind_rustls(lan, ingest_tls);
    let mut read_server = axum_server::bind_rustls(vpn, read_tls);
    // Polling clients reuse TLS, but abandoned sockets from a reboot must expire.
    ingest_server
        .http_builder()
        .http1()
        .timer(hyper_util::rt::TokioTimer::new())
        .header_read_timeout(Duration::from_secs(30));
    read_server
        .http_builder()
        .http1()
        .timer(hyper_util::rt::TokioTimer::new())
        .header_read_timeout(Duration::from_secs(30));
    tokio::try_join!(
        ingest_server.serve(ingest_app.into_make_service()),
        read_server.serve(read_app.into_make_service()),
    )?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Measurement {
        Measurement {
            node_id: "box3".into(),
            boot_id: "0123456789abcdef".into(),
            sequence: 1,
            temperature_mc: 23_450,
            humidity_milli_percent: 45_600,
            sample_age_ms: 200,
            uptime_ms: 10_000,
            interval_secs: 3600,
        }
    }

    #[test]
    fn accepts_valid_sensor_data() {
        assert!(sample().validate());
    }

    #[test]
    fn rejects_bad_identity_stale_data_and_out_of_range_values() {
        let mut value = sample();
        value.node_id = "other".into();
        assert!(!value.validate());
        value = sample();
        value.humidity_milli_percent = 100_001;
        assert!(!value.validate());
        value = sample();
        value.sample_age_ms = 15_001;
        assert!(!value.validate());
        value = sample();
        value.temperature_mc = 85_001;
        assert!(!value.validate());
        value = sample();
        value.sequence = 0;
        assert!(!value.validate());
        value = sample();
        value.boot_id = "not-a-hex-id".into();
        assert!(!value.validate());
    }

    #[tokio::test]
    async fn persists_valid_measurements_and_rejects_replays() {
        let directory = tempfile::tempdir().unwrap();
        let store = Arc::new(Store {
            path: directory.path().join("latest.json"),
            latest: RwLock::new(None),
            dashboard: Arc::new(RwLock::new(dashboard::DashboardState::default())),
        });
        assert_eq!(
            ingest(State(store.clone()), Json(sample())).await.unwrap(),
            StatusCode::NO_CONTENT
        );
        let saved: Snapshot = serde_json::from_slice(&fs::read(&store.path).unwrap()).unwrap();
        assert_eq!(saved.measurement.temperature_mc, 23_450);
        assert_eq!(
            ingest(State(store.clone()), Json(sample()))
                .await
                .unwrap_err(),
            StatusCode::CONFLICT
        );
        let Json(readback) = latest(State(store)).await.unwrap();
        assert!(!readback.stale);
        assert_eq!(readback.measurement.sequence, 1);
    }

    #[tokio::test]
    async fn dashboard_http_response_supports_forecast_and_preserves_small_frames() {
        for with_forecast in [false, true] {
            let mut dashboard = dashboard::DashboardState::default();
            if with_forecast {
                dashboard.forecast = Some(forecast::Cached {
                    received: unix_now().unwrap(),
                    data: forecast::Forecast {
                        ok: true,
                        age_secs: 0,
                        source_age_secs: 60,
                        temp_centi: 1400,
                        pressure_dhpa: 10190,
                        points: (0..48)
                            .filter(|i| *i < 23 && i % 2 == 0 || *i >= 23 && i % 2 == 1)
                            .map(|i| forecast::Point {
                                m: (i + 1) * 60,
                                t: 1400,
                                p: 10190,
                                lo: 1000,
                                hi: 1800,
                                pl: 10000,
                                ph: 10300,
                            })
                            .collect(),
                    },
                });
            }
            let store = Arc::new(Store {
                path: PathBuf::new(),
                latest: RwLock::new(None),
                dashboard: Arc::new(RwLock::new(dashboard)),
            });
            let response = device_dashboard(State(store)).await.into_response();
            assert_eq!(response.status(), StatusCode::OK);
            assert_eq!(response.headers()[header::CONTENT_TYPE], "application/json");
            let body = axum::body::to_bytes(response.into_body(), 4096)
                .await
                .unwrap();
            assert_eq!(body.len(), if with_forecast { 4096 } else { 1024 });
            let parsed: serde_json::Value = serde_json::from_slice(&body).unwrap();
            assert_eq!(parsed["forecast"]["ok"], with_forecast);
            if with_forecast {
                assert_eq!(parsed["forecast"]["points"].as_array().unwrap().len(), 25);
            }
        }
    }

    #[test]
    fn marks_missing_hourly_updates_stale() {
        assert!(!stale(3600, 3600));
        assert!(stale(5521, 3600));
    }
}
