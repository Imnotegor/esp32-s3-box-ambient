use serde::Serialize;
use serde_json::Value;

#[derive(Default, Serialize)]
pub struct BoxForecast {
    pub ok: bool,
    pub age_secs: i32,
    pub source_age_secs: i32,
    pub temp_centi: i32,
    pub pressure_dhpa: i32,
    pub points: Vec<Point>,
}

#[derive(Serialize)]
pub struct Point {
    pub m: i32,
    pub t: i32,
    pub p: i32,
    pub lo: i32,
    pub hi: i32,
    pub pl: i32,
    pub ph: i32,
}

fn scaled(value: &Value, factor: f64, minimum: i32, maximum: i32) -> Option<i32> {
    let n = (value.as_f64()? * factor).round();
    (n.is_finite() && n >= minimum as f64 && n <= maximum as f64).then_some(n as i32)
}

fn convert(state: &Value, now: i64) -> Option<BoxForecast> {
    let forecast = &state["forecast"];
    if forecast["forecast_type"] != "wttr_local_tcn" || forecast["future_forecast_inputs"] != false
    {
        return None;
    }
    let issued = forecast["generated_at"].as_i64()?;
    let source = forecast["initial_state_at"].as_i64()?;
    if source > issued
        || !(0..=7200).contains(&(now - issued))
        || !(0..=21600).contains(&(now - source))
    {
        return None;
    }
    let current = &forecast["current"]["values"];
    let hours = forecast["hourly"].as_array()?;
    if hours.len() != 48 {
        return None;
    }
    let mut output = BoxForecast {
        ok: true,
        age_secs: (now - issued) as i32,
        source_age_secs: (now - source) as i32,
        temp_centi: scaled(&current[0], 100.0, -9000, 6500)?,
        pressure_dhpa: scaled(&current[1], 10.0, 8500, 11000)?,
        points: vec![],
    };
    let mut previous = None;
    for (i, hour) in hours.iter().enumerate() {
        let at = hour["valid_at"].as_i64()?;
        if previous.is_some_and(|p| at - p != 3600) || at <= issued || at - issued > 48 * 3600 {
            return None;
        }
        previous = Some(at);
        // Keep both 24h/48h endpoints, with two-hour spacing elsewhere.
        if !(i < 23 && i % 2 == 0 || i >= 23 && i % 2 == 1) {
            continue;
        }
        let point = Point {
            m: ((at - issued + 59) / 60) as i32,
            t: scaled(&hour["temperature_c"], 100.0, -9000, 6500)?,
            p: scaled(&hour["pressure_hpa"], 10.0, 8500, 11000)?,
            lo: scaled(&hour["temperature_interval90"][0], 100.0, -12000, 9000)?,
            hi: scaled(&hour["temperature_interval90"][1], 100.0, -12000, 9000)?,
            pl: scaled(&hour["pressure_interval90_hpa"][0], 10.0, 6500, 13000)?,
            ph: scaled(&hour["pressure_interval90_hpa"][1], 10.0, 6500, 13000)?,
        };
        if point.lo > point.t || point.hi < point.t || point.pl > point.p || point.ph < point.p {
            return None;
        }
        output.points.push(point);
    }
    Some(output)
}

pub fn snapshot(state: &Value, now: i64) -> BoxForecast {
    convert(state, now).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn bounded_box_payload_preserves_both_horizons_and_rejects_stale_or_invalid_data() {
        let now = 1_800_000_000;
        let hours: Vec<_>=(1..=48).map(|h|json!({"valid_at":now+h*3600,"temperature_c":14.2,"pressure_hpa":1019.0,"temperature_interval90":[10.0,18.0],"pressure_interval90_hpa":[1015.0,1023.0]})).collect();
        let mut value = json!({"forecast":{"forecast_type":"wttr_local_tcn","future_forecast_inputs":false,"generated_at":now,"initial_state_at":now-3600,"current":{"values":[14.0,1019.0]},"hourly":hours}});
        let out = snapshot(&value, now);
        assert!(out.ok);
        assert_eq!(out.points.len(), 25);
        assert!(out.points.iter().any(|p| p.m == 1440));
        assert_eq!(out.points.last().unwrap().m, 2880);
        assert!(serde_json::to_vec(&out).unwrap().len() < 3072);
        assert!(!snapshot(&value, now + 7201).ok);
        value["forecast"]["hourly"][0]["pressure_interval90_hpa"][1] = 1000.into();
        assert!(!snapshot(&value, now).ok);
    }
}
