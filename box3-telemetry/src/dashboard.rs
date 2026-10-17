use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, fs, path::PathBuf, sync::Arc, time::Duration};
use tokio::sync::RwLock;

#[derive(Clone, Default)]
pub struct DashboardState {
    metrics: Option<Metrics>,
    weather: Option<Weather>,
}

pub type Shared = Arc<RwLock<DashboardState>>;

#[derive(Clone)]
struct Metrics {
    updated: u64,
    cpu: i32,
    mem_used: i32,
    mem_total: i32,
    swap_used: i32,
    swap_total: i32,
    uptime: i32,
    load: i32,
}

#[derive(Clone, Deserialize, Serialize)]
struct Weather {
    updated: u64,
    temp_mc: i32,
    feels_mc: i32,
    humidity: i32,
    kind: i32,
    text: String,
}

#[derive(Clone, Default, Serialize)]
pub struct DeviceDashboard {
    pub metrics_ok: bool,
    pub cpu_milli_percent: i32,
    pub mem_used_kib: i32,
    pub mem_total_kib: i32,
    pub swap_used_kib: i32,
    pub swap_total_kib: i32,
    pub uptime_secs: i32,
    pub load_milli: i32,
    pub metrics_age_secs: i32,
    pub weather_ok: bool,
    pub weather_temp_mc: i32,
    pub weather_feels_mc: i32,
    pub weather_humidity_percent: i32,
    pub weather_kind: i32,
    pub weather_text: String,
    pub weather_age_secs: i32,
    pub weather_stale: bool,
}

fn age(now: u64, then: u64) -> i32 {
    now.saturating_sub(then).min(i32::MAX as u64) as i32
}

pub async fn snapshot(shared: &Shared) -> DeviceDashboard {
    let now = crate::unix_now().unwrap_or(0);
    let state = shared.read().await;
    let mut out = DeviceDashboard::default();
    if let Some(m) = &state.metrics {
        out.metrics_age_secs = age(now, m.updated);
        out.metrics_ok = out.metrics_age_secs <= 15;
        out.cpu_milli_percent = m.cpu;
        out.mem_used_kib = m.mem_used;
        out.mem_total_kib = m.mem_total;
        out.swap_used_kib = m.swap_used;
        out.swap_total_kib = m.swap_total;
        out.uptime_secs = m.uptime;
        out.load_milli = m.load;
    }
    if let Some(w) = &state.weather {
        out.weather_ok = true;
        out.weather_age_secs = age(now, w.updated);
        out.weather_stale = out.weather_age_secs > 1800;
        out.weather_temp_mc = w.temp_mc;
        out.weather_feels_mc = w.feels_mc;
        out.weather_humidity_percent = w.humidity;
        out.weather_kind = w.kind;
        out.weather_text = w.text.clone();
    }
    out
}

fn cpu_times(text: &str) -> Option<(u64, u64)> {
    let mut fields = text.lines().next()?.split_whitespace();
    if fields.next()? != "cpu" {
        return None;
    }
    let values: Vec<u64> = fields
        .take(8)
        .map(str::parse)
        .collect::<Result<_, _>>()
        .ok()?;
    if values.len() < 5 {
        return None;
    }
    // guest/guest_nice are already included in user/nice, so do not add them again.
    Some((values.iter().sum(), values[3] + values[4]))
}

fn cpu_usage(previous: (u64, u64), current: (u64, u64)) -> Option<i32> {
    let total = current.0.checked_sub(previous.0)?;
    let idle = current.1.checked_sub(previous.1)?;
    if total == 0 || idle > total {
        return None;
    }
    Some((((total - idle) as u128 * 100_000) / total as u128) as i32)
}

fn memory(text: &str) -> Option<(i32, i32, i32, i32)> {
    let fields: HashMap<&str, u64> = text
        .lines()
        .filter_map(|line| {
            let (key, value) = line.split_once(':')?;
            Some((key, value.split_whitespace().next()?.parse().ok()?))
        })
        .collect();
    let total = *fields.get("MemTotal")?;
    let used = total.checked_sub(*fields.get("MemAvailable")?)?;
    let swap = *fields.get("SwapTotal")?;
    let swap_used = swap.checked_sub(*fields.get("SwapFree")?)?;
    Some((
        used.try_into().ok()?,
        total.try_into().ok()?,
        swap_used.try_into().ok()?,
        swap.try_into().ok()?,
    ))
}

fn current_metrics(cpu: i32) -> Result<Metrics> {
    let (mem_used, mem_total, swap_used, swap_total) =
        memory(&fs::read_to_string("/proc/meminfo")?).context("invalid meminfo")?;
    let uptime: u64 = fs::read_to_string("/proc/uptime")?
        .split('.')
        .next()
        .context("missing uptime")?
        .parse()?;
    let load: f64 = fs::read_to_string("/proc/loadavg")?
        .split_whitespace()
        .next()
        .context("missing load")?
        .parse()?;
    anyhow::ensure!(
        load.is_finite() && (0.0..1_000_000.0).contains(&load),
        "invalid load"
    );
    Ok(Metrics {
        updated: crate::unix_now()?,
        cpu,
        mem_used,
        mem_total,
        swap_used,
        swap_total,
        uptime: uptime.min(i32::MAX as u64) as i32,
        load: (load * 1000.0).round() as i32,
    })
}

async fn collect_metrics(state: Shared) {
    let mut previous = fs::read_to_string("/proc/stat")
        .ok()
        .and_then(|s| cpu_times(&s));
    loop {
        tokio::time::sleep(Duration::from_secs(2)).await;
        let current = fs::read_to_string("/proc/stat")
            .ok()
            .and_then(|s| cpu_times(&s));
        if let (Some(old), Some(new)) = (previous, current) {
            if let Some(cpu) = cpu_usage(old, new) {
                if let Ok(metrics) = current_metrics(cpu) {
                    state.write().await.metrics = Some(metrics);
                }
            }
        }
        previous = current;
    }
}

fn temperature(value: &str) -> Option<i32> {
    let number: i32 = value.trim().strip_suffix("°C")?.parse().ok()?;
    (-80..=65).contains(&number).then_some(number * 1000)
}

fn parse_weather(text: &str, updated: u64) -> Option<Weather> {
    let fields: Vec<&str> = text.trim().split('|').collect();
    if fields.len() != 4 {
        return None;
    }
    let temp_mc = temperature(fields[0])?;
    let feels_mc = temperature(fields[1])?;
    let humidity: i32 = fields[2].trim().strip_suffix('%')?.parse().ok()?;
    if !(0..=100).contains(&humidity) {
        return None;
    }
    let text: String = fields[3]
        .trim()
        .chars()
        .filter(|c| c.is_ascii_graphic() || *c == ' ')
        .take(40)
        .collect();
    if text.is_empty() {
        return None;
    }
    let lower = text.to_lowercase();
    let has = |words: &[&str]| words.iter().any(|word| lower.contains(word));
    let kind = if has(&["thunder", "storm"]) {
        4
    } else if has(&["snow", "sleet", "blizzard"]) {
        3
    } else if has(&["rain", "drizzle", "shower"]) {
        2
    } else if has(&["mist", "fog", "haze"]) {
        5
    } else if has(&["cloud", "overcast"]) {
        1
    } else if has(&["clear", "sun"]) {
        0
    } else {
        6
    };
    Some(Weather {
        updated,
        temp_mc,
        feels_mc,
        humidity,
        kind,
        text,
    })
}

async fn fetch_weather(client: &reqwest::Client, url: &str) -> Result<Weather> {
    let mut response = client
        .get(url)
        .query(&[("format", "%t|%f|%h|%C"), ("m", ""), ("lang", "en")])
        .send()
        .await?
        .error_for_status()?;
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await? {
        if body.len() + chunk.len() > 512 {
            bail!("weather response too large");
        }
        body.extend_from_slice(&chunk);
    }
    parse_weather(std::str::from_utf8(&body)?, crate::unix_now()?)
        .context("invalid metric weather response")
}

async fn collect_weather(state: Shared, path: PathBuf, url: String) {
    let client = match reqwest::Client::builder()
        .timeout(Duration::from_secs(8))
        .user_agent("box3-dashboard/0.2")
        .https_only(true)
        .build()
    {
        Ok(client) => client,
        Err(_) => return,
    };
    let mut backoff = 30;
    loop {
        let delay;
        match fetch_weather(&client, &url).await {
            Ok(weather) => {
                state.write().await.weather = Some(weather.clone());
                backoff = 30;
                delay = 600;
                let saved = weather;
                let path = path.clone();
                let _ =
                    tokio::task::spawn_blocking(move || crate::atomic_save(&path, &saved)).await;
            }
            Err(error) => {
                eprintln!(
                    "Weather refresh unavailable: {}",
                    if error.is::<reqwest::Error>() {
                        "HTTPS request failed"
                    } else {
                        "invalid response"
                    }
                );
                delay = backoff;
                backoff = (backoff * 2).min(600);
            }
        }
        tokio::time::sleep(Duration::from_secs(delay)).await;
    }
}

pub fn start(path: PathBuf, weather_url: String) -> Shared {
    let weather = fs::read(&path)
        .ok()
        .and_then(|b| serde_json::from_slice::<Weather>(&b).ok())
        .filter(|w| {
            (-80_000..=65_000).contains(&w.temp_mc)
                && (-80_000..=65_000).contains(&w.feels_mc)
                && (0..=100).contains(&w.humidity)
                && (0..=6).contains(&w.kind)
                && !w.text.is_empty()
                && w.text.len() <= 40
                && w.text.bytes().all(|b| b.is_ascii_graphic() || b == b' ')
                && w.updated <= crate::unix_now().unwrap_or(0) + 300
        });
    let state = Arc::new(RwLock::new(DashboardState {
        metrics: None,
        weather,
    }));
    tokio::spawn(collect_metrics(state.clone()));
    tokio::spawn(collect_weather(state.clone(), path, weather_url));
    state
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpu_delta_excludes_idle_and_guest_double_counting() {
        assert_eq!(cpu_times("cpu 10 0 10 70 10 0 0 0 9 0\n"), Some((100, 80)));
        assert_eq!(cpu_usage((100, 80), (200, 140)), Some(40_000));
        assert_eq!(cpu_usage((100, 80), (100, 80)), None);
        assert_eq!(cpu_usage((100, 80), (50, 40)), None);
    }

    #[test]
    fn memory_matches_available_and_swap_accounting() {
        assert_eq!(
            memory("MemTotal: 1000 kB\nMemAvailable: 600 kB\nSwapTotal: 100 kB\nSwapFree: 90 kB"),
            Some((400, 1000, 10, 100))
        );
        assert_eq!(memory("MemTotal: 100 kB"), None);
    }

    #[test]
    fn parses_metric_weather_and_rejects_bad_data() {
        let weather = parse_weather("+15°C|+12°C|80%|Partly cloudy", 100).unwrap();
        assert_eq!(
            (weather.temp_mc, weather.feels_mc, weather.kind),
            (15000, 12000, 1)
        );
        assert_eq!(
            parse_weather("+55°F|+50°F|80%|Clear", 100).map(|w| w.temp_mc),
            None
        );
        assert!(parse_weather("N/A|+12°C|80%|Clear", 100).is_none());
        assert!(parse_weather("+15°C|+12°C|101%|Clear", 100).is_none());
    }

    #[tokio::test]
    async fn empty_dashboard_does_not_invent_zero_degree_weather() {
        let state = Arc::new(RwLock::new(DashboardState::default()));
        let data = snapshot(&state).await;
        assert!(!data.metrics_ok);
        assert!(!data.weather_ok);
    }
}
