use crate::{
    history::Config,
    training,
    wttr::{self, Observation},
};
use anyhow::{ensure, Result};
use axum::{extract::State, http::header, routing::get, Json, Router};
use chrono::Utc;
use serde_json::{json, Value};
use std::{fs, net::SocketAddr, sync::Arc, time::Duration};
use tokio::sync::RwLock;

type Shared = Arc<RwLock<Value>>;

fn observation_json(o: &Observation) -> Value {
    json!({"at":o.at,"received_at":o.received_at,"timestamp_basis":o.timestamp_basis,
        "temperature_c":o.values[0],"pressure_hpa":o.values[1],"humidity_percent":o.values[2],
        "wind_ms":o.values[3].hypot(o.values[4]),"cloud_percent":o.values[5]})
}

fn compare(forecasts: &[Value], observations: &[Observation]) -> Value {
    let mut count = 0usize;
    let mut errors = [0.0f64; 2];
    for forecast in forecasts {
        let Some(issued) = forecast["generated_at"].as_i64() else {
            continue;
        };
        let Some(hours) = forecast["hourly"].as_array() else {
            continue;
        };
        for hour in hours {
            let Some(valid) = hour["valid_at"].as_i64() else {
                continue;
            };
            let observed = observations
                .iter()
                .filter(|o| o.at > issued && o.received_at > issued && (o.at - valid).abs() <= 1800)
                .min_by_key(|o| (o.at - valid).abs());
            if let (Some(o), Some(t), Some(p)) = (
                observed,
                hour["temperature_c"].as_f64(),
                hour["pressure_hpa"]
                    .as_f64()
                    .or_else(|| hour["pressure_msl_hpa"].as_f64()),
            ) {
                errors[0] += (t - o.values[0] as f64).abs();
                errors[1] += (p - o.values[1] as f64).abs();
                count += 1;
            }
        }
    }
    json!({"matched_points":count,"temperature_mae_c":if count>0 {Some(errors[0]/count as f64)} else {None},
        "pressure_mae_hpa":if count>0 {Some(errors[1]/count as f64)} else {None},
        "reference":"subsequent wttr current conditions","matching_tolerance_minutes":30})
}

fn archived_forecasts(config: &Config, since: i64) -> Result<Vec<Value>> {
    let directory = wttr::directory(config).join("forecasts");
    if !directory.exists() {
        return Ok(vec![]);
    }
    let mut forecasts = vec![];
    for entry in fs::read_dir(directory)? {
        let path = entry?.path();
        if path.extension().and_then(|s| s.to_str()) != Some("json") {
            continue;
        }
        let at = path
            .file_stem()
            .and_then(|s| s.to_str())
            .and_then(|s| s.split('-').next())
            .and_then(|s| s.parse::<i64>().ok());
        if at.is_none_or(|v| v < since) {
            continue;
        }
        forecasts.push(serde_json::from_slice(&fs::read(path)?)?);
    }
    Ok(forecasts)
}

fn snapshot(
    config: &Config,
    forecast: Option<Value>,
    error: Option<&str>,
    refresh_seconds: u64,
) -> Result<Value> {
    let now = Utc::now().timestamp();
    let observations = wttr::observations(config, now - 30 * 86400)?;
    let quality = compare(
        &archived_forecasts(config, now - 30 * 86400)?,
        &observations,
    );
    let current = observations.last().map(observation_json);
    let points: Vec<_> = observations
        .iter()
        .filter(|o| o.at >= now - 72 * 3600)
        .map(observation_json)
        .collect();
    let status = if error.is_some() {
        "error"
    } else if forecast.is_some() {
        "ready"
    } else {
        "collecting"
    };
    Ok(
        json!({"status":status,"server_now":now,"last_attempt_at":now,"error":error,"refresh_seconds":refresh_seconds,
        "current":current,"observations":points,"observation_count":observations.len(),"forecast":forecast,"quality":quality}),
    )
}

async fn data(
    State(state): State<Shared>,
) -> ([(header::HeaderName, &'static str); 1], Json<Value>) {
    let mut value = state.read().await.clone();
    value["server_now"] = json!(Utc::now().timestamp());
    let stale = value["forecast"]["initial_state_at"]
        .as_i64()
        .is_some_and(|at| Utc::now().timestamp() - at > 6 * 3600);
    value["forecast_stale"] = json!(stale);
    ([(header::CACHE_CONTROL, "no-store")], Json(value))
}

async fn box_data(State(state): State<Shared>) -> Json<crate::box_forecast::BoxForecast> {
    Json(crate::box_forecast::snapshot(
        &*state.read().await,
        Utc::now().timestamp(),
    ))
}

pub async fn serve(config: Config, bind: SocketAddr, refresh_seconds: u64) -> Result<()> {
    ensure!(bind.ip().is_loopback(), "bind must be loopback; use an SSH tunnel or an authenticated reverse proxy for remote access");
    ensure!(
        (300..=3600).contains(&refresh_seconds),
        "refresh_seconds must be 300..3600"
    );
    let site = wttr::directory(&config)
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap()
        .to_owned();
    let cache = config.state_dir.join("wttr-forecast.json");
    let previous = fs::read(cache)
        .ok()
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .filter(|v| v["site_key"].as_str() == Some(&site));
    let shared: Shared = Arc::new(RwLock::new(snapshot(
        &config,
        previous,
        None,
        refresh_seconds,
    )?));
    let state = shared.clone();
    let worker = tokio::spawn(async move {
        loop {
            let result = training::refresh(&config).await;
            let (forecast, error) = match result {
                Ok(value) => (Some(value), None),
                Err(error) => {
                    eprintln!("weather update failed: {error:#}");
                    let old = state.read().await["forecast"].clone();
                    let message = if !config.state_dir.join("active-wttr-model.json").exists() {
                        "Модель ещё не обучена. История wttr продолжает собираться."
                    } else {
                        "Обновить прогноз не удалось. Показаны последние сохранённые данные."
                    };
                    (if old.is_null() { None } else { Some(old) }, Some(message))
                }
            };
            match snapshot(&config, forecast, error, refresh_seconds) {
                Ok(value) => *state.write().await = value,
                Err(error) => {
                    eprintln!("weather archive failed: {error:#}");
                    let mut value = state.write().await;
                    value["status"] = json!("error");
                    value["error"] = json!("Не удалось прочитать историю погоды.");
                }
            }
            tokio::time::sleep(Duration::from_secs(refresh_seconds)).await;
        }
    });
    let app = Router::new()
        .route("/api/state", get(data))
        .route("/v1/box-forecast", get(box_data))
        .with_state(shared);
    let listener = tokio::net::TcpListener::bind(bind).await?;
    println!("BOX forecast API http://{bind}; source refresh every {refresh_seconds}s; CPU only");
    let result = axum::serve(listener, app)
        .with_graceful_shutdown(async {
            let _ = tokio::signal::ctrl_c().await;
        })
        .await;
    worker.abort();
    result?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn scoring_requires_observations_received_after_forecast() {
        let forecast = json!({"generated_at":10000,"hourly":[{"valid_at":13600,"temperature_c":12.0,"pressure_hpa":1010.0}]});
        let mut o = Observation {
            at: 13600,
            received_at: 14000,
            timestamp_basis: "first_received".into(),
            source_clock: String::new(),
            values: [14.0, 1013.0, 60.0, 0.0, 1.0, 50.0],
            fingerprint: String::new(),
        };
        let score = compare(std::slice::from_ref(&forecast), &[o.clone()]);
        assert_eq!(score["matched_points"], 1);
        assert_eq!(score["temperature_mae_c"], 2.0);
        assert_eq!(score["pressure_mae_hpa"], 3.0);
        o.received_at = 9000;
        assert_eq!(compare(&[forecast], &[o])["matched_points"], 0);
    }
}
