use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use std::{net::IpAddr, time::Duration};

#[derive(Clone, Default, Deserialize, Serialize)]
pub struct Forecast {
    pub ok: bool,
    pub age_secs: i32,
    pub source_age_secs: i32,
    pub temp_centi: i32,
    pub pressure_dhpa: i32,
    pub points: Vec<Point>,
}
#[derive(Clone, Deserialize, Serialize)]
pub struct Point {
    pub m: i32,
    pub t: i32,
    pub p: i32,
    pub lo: i32,
    pub hi: i32,
    pub pl: i32,
    pub ph: i32,
}

impl Forecast {
    fn valid(&self) -> bool {
        if !self.ok {
            return self.points.is_empty();
        }
        if self.points.len() != 25
            || self.age_secs < 0
            || self.source_age_secs < self.age_secs
            || !(-9000..=6500).contains(&self.temp_centi)
            || !(8500..=11000).contains(&self.pressure_dhpa)
        {
            return false;
        }
        let mut last = 0;
        for p in &self.points {
            if p.m <= last
                || p.m > 2880
                || last == 0 && p.m > 60
                || !(-9000..=6500).contains(&p.t)
                || !(8500..=11000).contains(&p.p)
                || p.lo < -12000
                || p.lo > p.t
                || p.hi < p.t
                || p.hi > 9000
                || p.pl < 6500
                || p.pl > p.p
                || p.ph < p.p
                || p.ph > 13000
            {
                return false;
            }
            last = p.m;
        }
        last >= 2760
    }
}

#[derive(Clone)]
pub struct Cached {
    pub data: Forecast,
    pub received: u64,
}
impl Cached {
    pub fn snapshot(&self, now: u64) -> Forecast {
        let mut data = self.data.clone();
        let elapsed = now.saturating_sub(self.received).min(i32::MAX as u64) as i32;
        data.age_secs = data.age_secs.saturating_add(elapsed);
        data.source_age_secs = data.source_age_secs.saturating_add(elapsed);
        if data.age_secs > 7200 || data.source_age_secs > 21600 {
            Forecast::default()
        } else {
            data
        }
    }
}

pub fn validate_url(value: &str) -> Result<reqwest::Url> {
    let url = reqwest::Url::parse(value)?;
    let address = url
        .host_str()
        .unwrap_or("")
        .trim_matches(['[', ']'])
        .parse::<IpAddr>();
    ensure!(
        url.scheme() == "http"
            && address.is_ok_and(|ip| ip.is_loopback())
            && url.username().is_empty()
            && url.password().is_none()
            && url.query().is_none()
            && url.path() == "/v1/box-forecast",
        "BOX3_FORECAST_URL must be a loopback HTTP /v1/box-forecast endpoint"
    );
    Ok(url)
}

pub async fn collect(state: crate::dashboard::Shared, url: reqwest::Url) {
    let client = match reqwest::Client::builder()
        .timeout(Duration::from_secs(3))
        .redirect(reqwest::redirect::Policy::none())
        .build()
    {
        Ok(client) => client,
        Err(_) => return,
    };
    loop {
        let result: Result<Forecast> = async {
            let mut response = client.get(url.clone()).send().await?.error_for_status()?;
            let mut bytes = vec![];
            while let Some(chunk) = response.chunk().await? {
                ensure!(
                    bytes.len() + chunk.len() <= 3072,
                    "forecast response too large"
                );
                bytes.extend(chunk);
            }
            let forecast: Forecast = serde_json::from_slice(&bytes)?;
            ensure!(forecast.valid(), "invalid forecast response");
            Ok(forecast)
        }
        .await;
        match result {
            Ok(data) => {
                state.write().await.forecast = Some(Cached {
                    data,
                    received: crate::unix_now().unwrap_or(0),
                })
            }
            Err(_) => eprintln!("Local CPU forecast unavailable; cached data will age normally"),
        }
        tokio::time::sleep(Duration::from_secs(30)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> Forecast {
        let points = (0..48)
            .filter(|i| *i < 23 && i % 2 == 0 || *i >= 23 && i % 2 == 1)
            .map(|i| Point {
                m: (i + 1) * 60,
                t: 1400,
                p: 10190,
                lo: 1000,
                hi: 1800,
                pl: 10000,
                ph: 10300,
            })
            .collect();
        Forecast {
            ok: true,
            age_secs: 30,
            source_age_secs: 60,
            temp_centi: 1400,
            pressure_dhpa: 10190,
            points,
        }
    }
    #[test]
    fn validates_bounds_and_ages_cached_forecasts_without_faking_freshness() {
        let mut data = fixture();
        assert!(data.valid());
        data.points[1].m = 1;
        assert!(!data.valid());
        let cached = Cached {
            data: fixture(),
            received: 100,
        };
        assert_eq!(cached.snapshot(160).age_secs, 90);
        assert!(!cached.snapshot(10000).ok);
    }
    #[test]
    fn endpoint_is_local_and_firmware_payload_is_bounded() {
        assert!(validate_url("http://127.0.0.1:8790/v1/box-forecast").is_ok());
        assert!(validate_url("http://192.0.2.1:8790/v1/box-forecast").is_err());
        assert!(validate_url("http://127.0.0.1:8790/wrong").is_err());
        let dashboard = crate::dashboard::DeviceDashboard {
            forecast: fixture(),
            weather_text: "x".repeat(40),
            ..Default::default()
        };
        assert!(serde_json::to_vec(&dashboard).unwrap().len() < 4096);
    }
}
