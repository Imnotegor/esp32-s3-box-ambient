use anyhow::{ensure, Context, Result};
use chrono::{DateTime, Datelike, Duration, NaiveDate, Timelike, Utc};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::Duration as StdDuration,
};

pub const GRID: usize = 9;
pub const VARIABLES: usize = 7;
pub const CHANNELS: usize = GRID * VARIABLES + 4;
pub const HISTORY: usize = 73;
pub const HORIZON: usize = 72;
pub const TARGETS: usize = 2;
pub const FIELDS: &str = "temperature_2m,pressure_msl,relative_humidity_2m,wind_speed_10m,wind_direction_10m,cloud_cover,precipitation";

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub latitude: f64,
    pub longitude: f64,
    pub state_dir: PathBuf,
    #[serde(default)]
    pub wttr_url: Option<String>,
}

impl Config {
    pub fn load(path: &Path) -> Result<Self> {
        let config: Self = serde_json::from_slice(&fs::read(path)?)?;
        ensure!(
            config.latitude.is_finite() && (-80.0..=80.0).contains(&config.latitude),
            "invalid latitude"
        );
        ensure!(
            config.longitude.is_finite() && (-170.0..=170.0).contains(&config.longitude),
            "invalid longitude"
        );
        ensure!(config.state_dir.is_absolute(), "state_dir must be absolute");
        if let Some(value) = &config.wttr_url {
            let url = reqwest::Url::parse(value)?;
            ensure!(
                url.scheme() == "https"
                    && url.host_str() == Some("wttr.in")
                    && url.username().is_empty()
                    && url.password().is_none()
                    && url.query().is_none(),
                "wttr reference must be an HTTPS location URL without credentials"
            );
        }
        fs::create_dir_all(config.cache())?;
        Ok(config)
    }
    pub fn fingerprint(&self) -> String {
        digest(
            format!(
                "direct-v1,{:.6},{:.6},grid3x3,lat3,lon5,{}",
                self.latitude, self.longitude, FIELDS
            )
            .as_bytes(),
        )
    }
    pub fn cache(&self) -> PathBuf {
        self.state_dir.join("history").join(self.fingerprint())
    }
    fn locations(&self) -> (String, String) {
        let mut lat = vec![];
        let mut lon = vec![];
        for y in -1..=1 {
            for x in -1..=1 {
                lat.push((self.latitude + y as f64 * 3.0).to_string());
                lon.push((self.longitude + x as f64 * 5.0).to_string());
            }
        }
        (lat.join(","), lon.join(","))
    }
}

pub fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
pub fn save(path: &Path, value: &impl Serialize) -> Result<()> {
    fs::create_dir_all(path.parent().context("path has no parent")?)?;
    let temporary = path.with_extension(format!("tmp-{}", std::process::id()));
    let mut file = fs::File::create(&temporary)?;
    file.write_all(&serde_json::to_vec(value)?)?;
    file.sync_all()?;
    fs::rename(temporary, path)?;
    Ok(())
}

#[derive(Deserialize, Serialize)]
pub struct Series {
    pub latitude: f64,
    pub longitude: f64,
    pub hourly_units: HashMap<String, String>,
    pub hourly: Hours,
}
#[derive(Deserialize, Serialize)]
pub struct Hours {
    pub time: Vec<i64>,
    pub temperature_2m: Vec<Option<f32>>,
    pub pressure_msl: Vec<Option<f32>>,
    pub relative_humidity_2m: Vec<Option<f32>>,
    pub wind_speed_10m: Vec<Option<f32>>,
    pub wind_direction_10m: Vec<Option<f32>>,
    pub cloud_cover: Vec<Option<f32>>,
    pub precipitation: Vec<Option<f32>>,
}

impl Series {
    fn validate(&self) -> Result<()> {
        let h = &self.hourly;
        ensure!(
            !h.time.is_empty() && h.time.windows(2).all(|p| p[1] - p[0] == 3600),
            "invalid hourly timeline"
        );
        for (name, unit) in [
            ("time", "unixtime"),
            ("temperature_2m", "°C"),
            ("pressure_msl", "hPa"),
            ("wind_speed_10m", "m/s"),
        ] {
            ensure!(
                self.hourly_units.get(name).map(String::as_str) == Some(unit),
                "invalid units for {name}"
            );
        }
        for column in [
            &h.temperature_2m,
            &h.pressure_msl,
            &h.relative_humidity_2m,
            &h.wind_speed_10m,
            &h.wind_direction_10m,
            &h.cloud_cover,
            &h.precipitation,
        ] {
            ensure!(
                column.len() == h.time.len() && column.iter().flatten().all(|v| v.is_finite()),
                "invalid data column"
            );
        }
        Ok(())
    }
    fn values(&self, i: usize) -> Option<[f32; VARIABLES]> {
        let h = &self.hourly;
        let t = h.temperature_2m[i]?;
        let p = h.pressure_msl[i]?;
        let rh = h.relative_humidity_2m[i]?;
        let speed = h.wind_speed_10m[i]?;
        let direction = h.wind_direction_10m[i]?;
        let cloud = h.cloud_cover[i]?;
        let rain = h.precipitation[i]?;
        if !(-90.0..=65.0).contains(&t)
            || !(850.0..=1100.0).contains(&p)
            || !(0.0..=100.0).contains(&rh)
            || !(0.0..=100.0).contains(&speed)
            || !(0.0..=360.0).contains(&direction)
            || !(0.0..=100.0).contains(&cloud)
            || !(0.0..=500.0).contains(&rain)
        {
            return None;
        }
        let angle = direction.to_radians();
        Some([
            t,
            p,
            rh,
            -speed * angle.sin(),
            -speed * angle.cos(),
            cloud,
            rain.ln_1p(),
        ])
    }
}

#[derive(Serialize, Deserialize)]
pub struct Dataset {
    pub fingerprint: String,
    pub source: String,
    pub times: Vec<i64>,
    pub values: Vec<f32>,
    pub excluded_hours: usize,
}
impl Dataset {
    pub fn target(&self, index: usize) -> [f32; TARGETS] {
        let offset = index * CHANNELS + (GRID / 2) * VARIABLES;
        [self.values[offset], self.values[offset + 1]]
    }
    pub fn validate(&self, config: &Config) -> Result<()> {
        ensure!(
            self.fingerprint == config.fingerprint(),
            "data belong to another grid"
        );
        ensure!(
            !self.times.is_empty() && self.values.len() == self.times.len() * CHANNELS,
            "invalid grid shape"
        );
        ensure!(
            self.times.windows(2).all(|t| t[1] > t[0]),
            "non-monotonic grid timestamps"
        );
        ensure!(
            self.values.iter().all(|v| v.is_finite()),
            "non-finite grid values"
        );
        Ok(())
    }
    pub fn window(&self, origin: usize) -> Result<HistoryWindow<'_>> {
        ensure!(
            origin >= HISTORY - 1 && origin < self.times.len(),
            "insufficient history"
        );
        let start = origin + 1 - HISTORY;
        ensure!(
            self.times[start..=origin]
                .windows(2)
                .all(|t| t[1] - t[0] == 3600),
            "gap in input history"
        );
        Ok(HistoryWindow {
            values: &self.values[start * CHANNELS..(origin + 1) * CHANNELS],
            first_at: self.times[start],
            last_at: self.times[origin],
        })
    }
    pub fn samples(&self) -> Vec<usize> {
        if self.times.len() < HISTORY + HORIZON {
            return vec![];
        }
        (HISTORY - 1..self.times.len() - HORIZON)
            .filter(|&i| {
                self.times[i] % (6 * 3600) == 0
                    && self.times[i + HORIZON] - self.times[i + 1 - HISTORY]
                        == ((HISTORY - 1 + HORIZON) * 3600) as i64
            })
            .collect()
    }
}

// This is the only weather input accepted by the model; no future targets or NWP forecast vector.
pub struct HistoryWindow<'a> {
    pub values: &'a [f32],
    pub first_at: i64,
    pub last_at: i64,
}
impl HistoryWindow<'_> {
    pub fn validate_at(&self, origin: i64) -> Result<()> {
        ensure!(
            self.last_at == origin && self.first_at == origin - (HISTORY as i64 - 1) * 3600,
            "input time boundary mismatch"
        );
        ensure!(
            self.values.len() == HISTORY * CHANNELS,
            "input history shape mismatch"
        );
        Ok(())
    }
}

fn append(
    config: &Config,
    dataset: &mut Dataset,
    series: &[Series],
    cutoff: Option<i64>,
) -> Result<()> {
    ensure!(series.len() == GRID, "expected nine grid locations");
    for (index, point) in series.iter().enumerate() {
        point.validate()?;
        ensure!(
            point.hourly.time == series[0].hourly.time,
            "grid time alignment mismatch"
        );
        let latitude = config.latitude + (index / 3) as f64 * 3.0 - 3.0;
        let longitude = config.longitude + (index % 3) as f64 * 5.0 - 5.0;
        ensure!(
            (point.latitude - latitude).abs() < 0.5 && (point.longitude - longitude).abs() < 0.5,
            "wrong grid location or order"
        );
    }
    for (i, &timestamp) in series[0].hourly.time.iter().enumerate() {
        if cutoff.is_some_and(|end| timestamp > end) {
            continue;
        }
        let Some(mut row) = series
            .iter()
            .map(|s| s.values(i))
            .collect::<Option<Vec<_>>>()
            .map(|points| points.into_iter().flatten().collect::<Vec<_>>())
        else {
            dataset.excluded_hours += 1;
            continue;
        };
        let time = DateTime::from_timestamp(timestamp, 0).context("invalid timestamp")?;
        let hour = time.hour() as f32 * std::f32::consts::TAU / 24.0;
        let season = time.ordinal0() as f32 * std::f32::consts::TAU / 365.25;
        row.extend([hour.sin(), hour.cos(), season.sin(), season.cos()]);
        dataset.times.push(timestamp);
        dataset.values.extend(row);
    }
    Ok(())
}

async fn fetch(
    config: &Config,
    start: NaiveDate,
    end: NaiveDate,
    provider: &str,
) -> Result<Vec<Series>> {
    let (lat, lon) = config.locations();
    let client = reqwest::Client::builder()
        .https_only(true)
        .timeout(StdDuration::from_secs(90))
        .build()?;
    for attempt in 0..3 {
        let response = client
            .get("https://archive-api.open-meteo.com/v1/archive")
            .query(&[
                ("latitude", lat.as_str()),
                ("longitude", lon.as_str()),
                ("start_date", &start.to_string()),
                ("end_date", &end.to_string()),
                ("hourly", FIELDS),
                ("models", provider),
                ("timeformat", "unixtime"),
                ("wind_speed_unit", "ms"),
            ])
            .send()
            .await;
        if let Ok(mut response) = response {
            if response.status().is_success() {
                let mut bytes = vec![];
                while let Some(chunk) = response.chunk().await? {
                    ensure!(
                        bytes.len() + chunk.len() <= 16_000_000,
                        "grid response too large"
                    );
                    bytes.extend(chunk);
                }
                let data: Vec<Series> = serde_json::from_slice(&bytes)?;
                for point in &data {
                    point.validate()?;
                }
                return Ok(data);
            }
            if response.status().as_u16() == 429 {
                let pause = response
                    .headers()
                    .get("retry-after")
                    .and_then(|h| h.to_str().ok())
                    .and_then(|s| s.parse::<u64>().ok())
                    .unwrap_or(60);
                ensure!(
                    pause <= 60,
                    "provider requests a longer pause; resume collection later"
                );
                eprintln!("provider rate limit; waiting {pause}s");
                tokio::time::sleep(StdDuration::from_secs(pause)).await;
                continue;
            }
            ensure!(
                response.status().is_server_error() || response.status().as_u16() == 429,
                "provider returned HTTP {}",
                response.status()
            );
        }
        tokio::time::sleep(StdDuration::from_secs(2 << attempt)).await;
    }
    anyhow::bail!("history provider unavailable")
}

pub async fn collect(config: &Config, start: NaiveDate, end: NaiveDate) -> Result<()> {
    ensure!(
        end > start && end <= Utc::now().date_naive() - Duration::days(7),
        "invalid reanalysis period"
    );
    let mut dataset = Dataset {
        fingerprint: config.fingerprint(),
        source: "ERA5 retrospective surface analyses".into(),
        times: vec![],
        values: vec![],
        excluded_hours: 0,
    };
    let mut from = start;
    while from <= end {
        let next = if from.month() == 12 {
            NaiveDate::from_ymd_opt(from.year() + 1, 1, 1)
        } else {
            NaiveDate::from_ymd_opt(from.year(), from.month() + 1, 1)
        }
        .context("invalid month")?;
        let to = (next - Duration::days(1)).min(end);
        let path = config.cache().join(format!("era5-{from}-{to}.json"));
        let cached = path.exists();
        let series: Vec<Series> = if cached {
            serde_json::from_slice(&fs::read(&path)?)?
        } else {
            let series = fetch(config, from, to, "era5").await?;
            save(&path, &series)?;
            series
        };
        append(config, &mut dataset, &series, None)?;
        println!(
            "history through={to} hours={} excluded={}",
            dataset.times.len(),
            dataset.excluded_hours
        );
        from = next;
        if !cached {
            tokio::time::sleep(StdDuration::from_secs(4)).await;
        }
    }
    dataset.validate(config)?;
    save(&config.state_dir.join("dataset.json"), &dataset)?;
    println!(
        "DATASET_READY hours={} eligible_origins={}",
        dataset.times.len(),
        dataset.samples().len()
    );
    Ok(())
}

pub fn load(config: &Config) -> Result<(Dataset, String)> {
    let bytes = fs::read(config.state_dir.join("dataset.json"))?;
    let dataset: Dataset = serde_json::from_slice(&bytes)?;
    dataset.validate(config)?;
    Ok((dataset, digest(&bytes)))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn model_window_cannot_contain_future_weather() {
        let values = vec![0.0; HISTORY * CHANNELS];
        let origin = 1_704_326_400;
        let window = HistoryWindow {
            values: &values,
            first_at: origin - (HISTORY as i64 - 1) * 3600,
            last_at: origin,
        };
        assert!(window.validate_at(origin).is_ok());
        assert!(window.validate_at(origin - 3600).is_err());
    }
    #[test]
    fn rejects_discontinuous_history() {
        let mut d = Dataset {
            fingerprint: "fixture".into(),
            source: "fixture".into(),
            times: (0..HISTORY).map(|i| i as i64 * 3600).collect(),
            values: vec![0.0; HISTORY * CHANNELS],
            excluded_hours: 0,
        };
        assert!(d.window(HISTORY - 1).is_ok());
        d.times[10] += 60;
        assert!(d.window(HISTORY - 1).is_err());
    }
}
