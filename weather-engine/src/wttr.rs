use crate::history::{self, Config, Dataset, CHANNELS, GRID, HISTORY, VARIABLES};
use anyhow::{ensure, Context, Result};
use chrono::{DateTime, NaiveDateTime, Utc};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{collections::BTreeMap, fs, time::Duration};

pub const WEATHER_FIELDS: usize = 6;
pub const HISTORY_MODES: [usize; 4] = [1, 6, 24, HISTORY];

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Observation {
    pub at: i64,
    pub received_at: i64,
    pub timestamp_basis: String,
    pub source_clock: String,
    pub values: [f32; WEATHER_FIELDS],
    pub fingerprint: String,
}

impl Observation {
    pub fn validate(&self) -> Result<()> {
        let [t, p, rh, u, v, cloud] = self.values;
        ensure!(
            (-90.0..=65.0).contains(&t)
                && (850.0..=1100.0).contains(&p)
                && (0.0..=100.0).contains(&rh)
                && (-100.0..=100.0).contains(&u)
                && (-100.0..=100.0).contains(&v)
                && (0.0..=100.0).contains(&cloud),
            "invalid wttr weather values"
        );
        ensure!(
            self.at <= self.received_at && self.at > 0,
            "invalid observation timestamp"
        );
        Ok(())
    }
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Input {
    pub origin: i64,
    pub rows: Vec<Option<[f32; WEATHER_FIELDS]>>,
}

impl Input {
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.rows.len() == HISTORY && self.rows[HISTORY - 1].is_some(),
            "missing current weather input"
        );
        ensure!(
            self.rows.iter().flatten().flatten().all(|v| v.is_finite()),
            "non-finite model input"
        );
        Ok(())
    }
    pub fn points(&self) -> usize {
        self.rows.iter().flatten().count()
    }
    pub fn mode(&self) -> usize {
        HISTORY_MODES
            .iter()
            .rposition(|n| self.points() >= *n)
            .unwrap_or(0)
    }
    pub fn historical(data: &Dataset, origin: usize, history_points: usize) -> Result<Self> {
        data.window(origin)?.validate_at(data.times[origin])?;
        ensure!(
            (1..=HISTORY).contains(&history_points),
            "invalid history size"
        );
        let mut rows = vec![None; HISTORY];
        for (slot, row) in rows.iter_mut().enumerate().skip(HISTORY - history_points) {
            let index = origin + slot + 1 - HISTORY;
            let offset = index * CHANNELS + (GRID / 2) * VARIABLES;
            *row = Some(data.values[offset..offset + WEATHER_FIELDS].try_into()?);
        }
        Ok(Self {
            origin: data.times[origin],
            rows,
        })
    }
    pub fn from_observations(observations: &[Observation], as_of: i64) -> Result<Self> {
        let current = observations
            .iter()
            .filter(|o| o.at <= as_of && o.received_at <= as_of)
            .max_by_key(|o| (o.at, o.received_at))
            .context("no wttr observations available")?;
        let origin = current.at;
        let mut rows = vec![None; HISTORY];
        let mut selected = [i64::MIN; HISTORY];
        for o in observations {
            if o.at > origin || o.received_at > as_of {
                continue;
            }
            let age = (origin - o.at + 1800) / 3600;
            if age >= HISTORY as i64 {
                continue;
            }
            let slot = HISTORY - 1 - age as usize;
            if o.at >= selected[slot] {
                rows[slot] = Some(o.values);
                selected[slot] = o.at;
            }
        }
        let input = Self { origin, rows };
        input.validate()?;
        Ok(input)
    }
}

pub fn directory(config: &Config) -> std::path::PathBuf {
    let id = history::digest(
        format!(
            "{}:{}",
            config.fingerprint(),
            config.wttr_url.as_deref().unwrap_or("")
        )
        .as_bytes(),
    );
    config.state_dir.join("wttr").join(id)
}

pub fn observations(config: &Config, since: i64) -> Result<Vec<Observation>> {
    let path = directory(config).join("observations");
    if !path.exists() {
        return Ok(vec![]);
    }
    let mut rows = BTreeMap::new();
    for entry in fs::read_dir(path)? {
        let path = entry?.path();
        if path.extension().and_then(|s| s.to_str()) != Some("json") {
            continue;
        }
        let at = path
            .file_stem()
            .and_then(|s| s.to_str())
            .and_then(|s| s.split('-').next())
            .and_then(|s| s.parse::<i64>().ok());
        if at.is_none_or(|at| at < since) {
            continue;
        }
        let observation: Observation = serde_json::from_slice(&fs::read(path)?)?;
        observation.validate()?;
        let replace = rows
            .get(&observation.at)
            .is_none_or(|old: &Observation| old.received_at < observation.received_at);
        if replace {
            rows.insert(observation.at, observation);
        }
    }
    Ok(rows.into_values().collect())
}

fn number(value: &Value, key: &str) -> Result<f32> {
    let value = value
        .get(key)
        .context(format!("missing wttr field {key}"))?;
    let parsed = if let Some(s) = value.as_str() {
        s.parse::<f32>()?
    } else {
        value.as_f64().context("invalid number")? as f32
    };
    ensure!(parsed.is_finite(), "non-finite wttr field");
    Ok(parsed)
}

pub fn parse(config: &Config, raw: &Value, received_at: i64) -> Result<Observation> {
    let area = raw
        .get("nearest_area")
        .and_then(Value::as_array)
        .and_then(|v| v.first())
        .context("missing wttr location")?;
    ensure!(
        (number(area, "latitude")? as f64 - config.latitude).abs() <= 0.5
            && (number(area, "longitude")? as f64 - config.longitude).abs() <= 0.5,
        "wttr returned a different location"
    );
    let current = raw
        .get("current_condition")
        .and_then(Value::as_array)
        .and_then(|v| v.first())
        .context("missing wttr current conditions")?;
    let speed = number(current, "windspeedKmph")? / 3.6;
    let direction = number(current, "winddirDegree")?;
    ensure!(
        (0.0..=100.0).contains(&speed) && (0.0..=360.0).contains(&direction),
        "invalid wind"
    );
    let values = [
        number(current, "temp_C")?,
        number(current, "pressure")?,
        number(current, "humidity")?,
        -speed * direction.to_radians().sin(),
        -speed * direction.to_radians().cos(),
        number(current, "cloudcover")?,
    ];
    let clock = current
        .get("observation_time")
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_owned();
    // A clock without a date/timezone is not a trustworthy UTC timestamp.
    let explicit = current
        .get("utcDateTime")
        .and_then(Value::as_str)
        .map(|s| {
            NaiveDateTime::parse_from_str(s, "%Y-%m-%d %H:%M").map(|t| t.and_utc().timestamp())
        })
        .transpose()?;
    let fingerprint = history::digest(&serde_json::to_vec(&(values, &clock, explicit))?);
    let observation = Observation {
        at: explicit.unwrap_or(received_at),
        received_at,
        timestamp_basis: if explicit.is_some() {
            "source_utc"
        } else {
            "first_received"
        }
        .into(),
        source_clock: clock,
        values,
        fingerprint,
    };
    observation.validate()?;
    ensure!(
        received_at - observation.at <= 6 * 3600,
        "wttr observation is stale"
    );
    Ok(observation)
}

pub fn ingest(config: &Config, raw: &Value, received_at: i64) -> Result<Observation> {
    let candidate = parse(config, raw, received_at)?;
    let directory = directory(config);
    let latest = directory.join("latest.json");
    if latest.exists() {
        let prior: Observation = serde_json::from_slice(&fs::read(&latest)?)?;
        prior.validate()?;
        if prior.fingerprint == candidate.fingerprint {
            return Ok(prior);
        }
        ensure!(
            candidate.at >= prior.at,
            "wttr returned an older observation"
        );
    }
    let name = format!("{}-{}.json", candidate.at, &candidate.fingerprint[..16]);
    history::save(&directory.join("observations").join(&name), &candidate)?;
    history::save(
        &directory.join("raw").join(name),
        &serde_json::json!({"received_at":received_at,"response":raw}),
    )?;
    history::save(&latest, &candidate)?;
    Ok(candidate)
}

pub async fn fetch(config: &Config) -> Result<Observation> {
    let url = config
        .wttr_url
        .as_ref()
        .context("wttr_url is required for live forecasts")?;
    let client = reqwest::Client::builder()
        .https_only(true)
        .timeout(Duration::from_secs(15))
        .build()?;
    let mut response = client
        .get(url)
        .query(&[("format", "j1")])
        .send()
        .await?
        .error_for_status()?;
    let mut bytes = vec![];
    while let Some(chunk) = response.chunk().await? {
        ensure!(
            bytes.len() + chunk.len() <= 1_000_000,
            "wttr response too large"
        );
        bytes.extend(chunk);
    }
    ingest(
        config,
        &serde_json::from_slice::<Value>(&bytes)?,
        Utc::now().timestamp(),
    )
}

pub fn calendar(at: i64) -> Result<[f32; 4]> {
    use chrono::{Datelike, Timelike};
    let time = DateTime::from_timestamp(at, 0).context("invalid input timestamp")?;
    let hour = (time.hour() as f32 + time.minute() as f32 / 60.0) * std::f32::consts::TAU / 24.0;
    let season = time.ordinal0() as f32 * std::f32::consts::TAU / 365.25;
    Ok([hour.sin(), hour.cos(), season.sin(), season.cos()])
}

#[cfg(test)]
mod tests {
    use super::*;
    fn config() -> Config {
        Config {
            latitude: 10.0,
            longitude: 20.0,
            state_dir: std::env::temp_dir(),
            wttr_url: Some("https://wttr.in/10,20".into()),
        }
    }
    fn raw() -> Value {
        serde_json::json!({"nearest_area":[{"latitude":"10","longitude":"20"}],"current_condition":[{"temp_C":"12","pressure":"1012","humidity":"70","windspeedKmph":"18","winddirDegree":"90","cloudcover":"20","observation_time":"08:00 AM"}],"weather":[{"hourly":[{"tempC":"30"}]}]})
    }
    #[test]
    fn current_weather_drives_features_and_future_forecast_does_not() -> Result<()> {
        let mut data = raw();
        let first = parse(&config(), &data, 1_800_000_000)?;
        data["weather"][0]["hourly"][0]["tempC"] = "-80".into();
        assert_eq!(first.values, parse(&config(), &data, first.at)?.values);
        data["current_condition"][0]["temp_C"] = "15".into();
        assert_ne!(first.values, parse(&config(), &data, first.at)?.values);
        assert_eq!(first.timestamp_basis, "first_received");
        assert!((first.values[3] + 5.0).abs() < 0.0001);
        Ok(())
    }
    #[test]
    fn sparse_history_preserves_missing_hours_and_excludes_future() -> Result<()> {
        let mut first = parse(&config(), &raw(), 1_800_000_000)?;
        let mut old = first.clone();
        old.at -= 3 * 3600;
        old.received_at = old.at;
        let mut future = first.clone();
        future.at += 3600;
        future.received_at = future.at;
        let input = Input::from_observations(&[old, first.clone(), future], first.at)?;
        assert_eq!(input.points(), 2);
        assert!(input.rows[HISTORY - 2].is_none());
        first.values[1] = 0.0;
        assert!(first.validate().is_err());
        Ok(())
    }

    #[test]
    fn duplicate_source_response_does_not_create_history_or_reset_its_age() -> Result<()> {
        let path =
            std::env::temp_dir().join(format!("box3-wttr-test-{:032x}", rand::random::<u128>()));
        fs::create_dir(&path)?;
        let mut config = config();
        config.state_dir = path.clone();
        let first = ingest(&config, &raw(), 1_800_000_000)?;
        let repeated = ingest(&config, &raw(), first.received_at + 3600)?;
        assert_eq!(first.at, repeated.at);
        assert_eq!(first.received_at, repeated.received_at);
        assert_eq!(observations(&config, 0)?.len(), 1);
        let mut changed = raw();
        changed["current_condition"][0]["temp_C"] = "13".into();
        ingest(&config, &changed, first.received_at + 7200)?;
        assert_eq!(observations(&config, 0)?.len(), 2);
        fs::remove_dir_all(path)?;
        Ok(())
    }
}
