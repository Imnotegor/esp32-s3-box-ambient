use crate::{
    history::{self, Config, Dataset, HISTORY, HORIZON, TARGETS},
    network::{self, Network, Normalization, INPUT_CHANNELS},
    wttr::{self, Input, HISTORY_MODES},
};
use anyhow::{ensure, Context, Result};
use candle_core::{Device, Tensor};
use candle_nn::{Optimizer, VarMap};
use chrono::Utc;
use rand::{rngs::StdRng, seq::SliceRandom, SeedableRng};
use serde::{Deserialize, Serialize};
use std::{fs, path::Path, time::Instant};

pub(crate) const BINS: [usize; 5] = [6, 12, 24, 48, 72];
pub(crate) fn bin(h: usize) -> usize {
    BINS.iter().position(|v| h <= *v).unwrap_or(4)
}

pub struct Splits {
    pub train: Vec<usize>,
    pub tuning: Vec<usize>,
    pub calibration: Vec<usize>,
    pub test: Vec<usize>,
    pub cutoffs: [i64; 3],
}
pub fn split(data: &Dataset) -> Result<Splits> {
    let samples = data.samples();
    ensure!(
        samples.len() >= 3000,
        "at least 3000 complete historical forecast windows required"
    );
    let first = data.times[*samples.first().unwrap()];
    let last = data.times[*samples.last().unwrap()];
    let span = last - first;
    let cuts = [
        first + span * 70 / 100,
        first + span * 80 / 100,
        first + span * 90 / 100,
    ];
    let mut result = Splits {
        train: vec![],
        tuning: vec![],
        calibration: vec![],
        test: vec![],
        cutoffs: cuts,
    };
    for index in samples {
        let t = data.times[index];
        let end = data.times[index + HORIZON];
        if end < cuts[0] {
            result.train.push(index);
        } else if t >= cuts[0] && end < cuts[1] {
            result.tuning.push(index);
        } else if t >= cuts[1] && end < cuts[2] {
            result.calibration.push(index);
        } else if t >= cuts[2] {
            result.test.push(index);
        }
    }
    ensure!(
        [
            result.train.len(),
            result.tuning.len(),
            result.calibration.len(),
            result.test.len()
        ]
        .iter()
        .all(|n| *n >= 200),
        "time partitions are too small"
    );
    Ok(result)
}

fn validation_loss(
    net: &Network,
    data: &Dataset,
    indices: &[usize],
    normal: &Normalization,
) -> Result<f32> {
    let mut total = 0.0;
    for batch in indices.chunks(32) {
        let (x, y) = network::batch(data, batch, normal, 0)?;
        let loss = candle_nn::loss::mse(&net.forward(&x)?, &y)?.to_scalar::<f32>()?;
        ensure!(loss.is_finite(), "non-finite validation loss");
        total += loss * batch.len() as f32;
    }
    Ok(total / indices.len() as f32)
}

fn fit(
    data: &Dataset,
    splits: &Splits,
    normal: &Normalization,
    seed: u64,
    epochs: usize,
    path: &Path,
) -> Result<Network> {
    let mut variables = VarMap::new();
    let net = Network::new(&variables)?;
    network::initialize(&variables, seed)?;
    let mut optimizer = candle_nn::AdamW::new(
        variables.all_vars(),
        candle_nn::ParamsAdamW {
            lr: 0.001,
            weight_decay: 0.001,
            ..Default::default()
        },
    )?;
    let mut rng = StdRng::seed_from_u64(seed);
    let mut indices = splits.train.clone();
    let mut best = f32::INFINITY;
    let mut improved = 0;
    for epoch in 0..epochs {
        let phase = std::f64::consts::PI * epoch as f64 / epochs.saturating_sub(1).max(1) as f64;
        optimizer.set_learning_rate(0.00005 + 0.00095 * (1.0 + phase.cos()) * 0.5);
        let clock = Instant::now();
        indices.shuffle(&mut rng);
        let mut training = 0.0;
        for (step, batch) in indices.chunks(32).enumerate() {
            let (x, y) = network::batch(data, batch, normal, epoch + seed as usize)?;
            let loss = candle_nn::loss::mse(&net.forward(&x)?, &y)?;
            training += loss.to_scalar::<f32>()? * batch.len() as f32;
            optimizer.backward_step(&loss)?;
            if (step + 1).is_multiple_of(100) {
                println!(
                    "seed={seed} epoch={} batches={}/{}",
                    epoch + 1,
                    step + 1,
                    indices.len().div_ceil(32)
                );
            }
        }
        let loss = validation_loss(&net, data, &splits.tuning, normal)?;
        if loss < best - 0.00001 {
            best = loss;
            improved = epoch;
            variables.save(path)?;
        }
        println!(
            "seed={seed} epoch={} train={:.5} tuning={loss:.5} seconds={:.1}",
            epoch + 1,
            training / indices.len() as f32,
            clock.elapsed().as_secs_f32()
        );
        if epoch >= 20 && epoch - improved >= 10 {
            break;
        }
    }
    variables.load(path)?;
    Ok(net)
}

pub(crate) fn predict(
    networks: &[Network],
    normal: &Normalization,
    input: &Input,
) -> Result<Vec<[f32; TARGETS]>> {
    let input = normal.encode(input)?;
    let tensor = Tensor::from_vec(input, (1, INPUT_CHANNELS, HISTORY), &Device::Cpu)?;
    let mut forecast = vec![[0.0; TARGETS]; HORIZON];
    for network in networks {
        let output = network.forward(&tensor)?.squeeze(0)?.to_vec1::<f32>()?;
        for (h, values) in output.chunks_exact(TARGETS).enumerate() {
            let values = normal.decode([values[0], values[1]]);
            for (j, value) in values.iter().enumerate() {
                forecast[h][j] += value / networks.len() as f32;
            }
        }
    }
    ensure!(
        forecast.iter().flatten().all(|x| x.is_finite()),
        "non-finite forecast"
    );
    Ok(forecast)
}

#[derive(Clone, Default, Serialize, Deserialize)]
pub struct Metric {
    pub count: [usize; 5],
    pub mae: [[f32; TARGETS]; 5],
    pub rmse: [[f32; TARGETS]; 5],
}
impl Metric {
    pub(crate) fn add(&mut self, lead: usize, value: [f32; TARGETS], truth: [f32; TARGETS]) {
        let b = bin(lead);
        self.count[b] += 1;
        for j in 0..TARGETS {
            let error = value[j] - truth[j];
            self.mae[b][j] += error.abs();
            self.rmse[b][j] += error * error;
        }
    }
    pub(crate) fn finish(&mut self) {
        for b in 0..5 {
            for j in 0..TARGETS {
                self.mae[b][j] /= self.count[b] as f32;
                self.rmse[b][j] = (self.rmse[b][j] / self.count[b] as f32).sqrt();
            }
        }
    }
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Evaluation {
    pub neural: Metric,
    pub persistence: Metric,
    pub repeat_previous_day: Metric,
}
#[derive(Serialize)]
struct TestForecast {
    origin: i64,
    values: Vec<[f32; TARGETS]>,
}

fn evaluate(
    networks: &[Network],
    normal: &Normalization,
    data: &Dataset,
    indices: &[usize],
    history_points: usize,
) -> Result<(Evaluation, Vec<TestForecast>)> {
    let mut result = Evaluation {
        neural: Metric::default(),
        persistence: Metric::default(),
        repeat_previous_day: Metric::default(),
    };
    let mut forecasts = vec![];
    for &origin in indices {
        let predicted = predict(
            networks,
            normal,
            &Input::historical(data, origin, history_points)?,
        )?;
        let last = data.target(origin);
        for h in 1..=HORIZON {
            let truth = data.target(origin + h);
            let daily = data.target(origin - 24 + (h - 1) % 24 + 1);
            result.neural.add(h, predicted[h - 1], truth);
            result.persistence.add(h, last, truth);
            result.repeat_previous_day.add(h, daily, truth);
        }
        forecasts.push(TestForecast {
            origin: data.times[origin],
            values: predicted,
        });
    }
    result.neural.finish();
    result.persistence.finish();
    result.repeat_previous_day.finish();
    Ok((result, forecasts))
}

#[derive(Clone, Serialize, Deserialize)]
pub struct ModeReport {
    pub history_points: usize,
    pub tuning: Evaluation,
    pub holdout: Evaluation,
    pub coverage90: [[f32; TARGETS]; 5],
}
#[derive(Clone, Serialize, Deserialize)]
pub struct Report {
    pub input_contract: String,
    pub source: String,
    pub split_cutoffs: [i64; 3],
    pub split_sizes: [usize; 4],
    pub lead_bins: [usize; 5],
    pub history_modes: Vec<ModeReport>,
    pub limitations: String,
}
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct Member {
    pub filename: String,
    pub sha256: String,
    pub seed: u64,
}
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct Manifest {
    pub version: u32,
    pub fingerprint: String,
    pub dataset_sha256: String,
    pub trained_at: i64,
    pub normal: Normalization,
    pub members: Vec<Member>,
    pub radii: [[[f32; TARGETS]; 5]; 4],
    pub report: Report,
}

pub fn train(config: &Config, epochs: usize) -> Result<()> {
    ensure!((1..=200).contains(&epochs), "epochs must be 1..200");
    let (data, checksum) = history::load(config)?;
    let splits = split(&data)?;
    let normal = Normalization::fit(&data, splits.cutoffs[0])?;
    let generation = format!("{}-{}", Utc::now().timestamp(), std::process::id());
    let directory = config.state_dir.join("wttr-models").join(&generation);
    fs::create_dir_all(&directory)?;
    let mut networks = vec![];
    let mut members = vec![];
    for (i, seed) in [42, 1729, 271828].into_iter().enumerate() {
        let filename = format!("direct-{i}.safetensors");
        let path = directory.join(&filename);
        fit(&data, &splits, &normal, seed, epochs, &path)?;
        networks.push(Network::load(&path)?);
        members.push(Member {
            filename,
            sha256: history::digest(&fs::read(path)?),
            seed,
        });
    }
    let mut radii = [[[0.0; TARGETS]; 5]; 4];
    let mut modes = vec![];
    for (mode, history_points) in HISTORY_MODES.into_iter().enumerate() {
        let (tuning, _) = evaluate(&networks, &normal, &data, &splits.tuning, history_points)?;
        let mut errors: Vec<Vec<Vec<f32>>> = (0..5).map(|_| vec![vec![], vec![]]).collect();
        for &origin in &splits.calibration {
            let forecast = predict(
                &networks,
                &normal,
                &Input::historical(&data, origin, history_points)?,
            )?;
            for h in 1..=HORIZON {
                let truth = data.target(origin + h);
                for (j, value) in truth.iter().enumerate() {
                    errors[bin(h)][j].push((forecast[h - 1][j] - value).abs());
                }
            }
        }
        for (b, row) in errors.iter_mut().enumerate() {
            for (j, values) in row.iter_mut().enumerate() {
                ensure!(values.len() > 100, "insufficient calibration data");
                values.sort_by(f32::total_cmp);
                let rank =
                    (((values.len() + 1) as f32 * 0.9).ceil() as usize).min(values.len()) - 1;
                radii[mode][b][j] = values[rank];
            }
        }
        let (holdout, forecasts) =
            evaluate(&networks, &normal, &data, &splits.test, history_points)?;
        let mut coverage = [[0.0; TARGETS]; 5];
        for (&origin, forecast) in splits.test.iter().zip(&forecasts) {
            for h in 1..=HORIZON {
                let truth = data.target(origin + h);
                for (j, value) in truth.iter().enumerate() {
                    if (forecast.values[h - 1][j] - value).abs() <= radii[mode][bin(h)][j] {
                        coverage[bin(h)][j] += 1.0;
                    }
                }
            }
        }
        for (b, row) in coverage.iter_mut().enumerate() {
            for value in row {
                *value /= holdout.neural.count[b] as f32;
            }
        }
        history::save(
            &directory.join(format!("test-{history_points}-points.json")),
            &forecasts,
        )?;
        modes.push(ModeReport {
            history_points,
            tuning,
            holdout,
            coverage90: coverage,
        });
    }
    let report = Report {
        input_contract: "Six local current-weather fields, calendar coordinates and an explicit missing-hour mask; future provider forecasts are never inputs".into(),
        source: data.source.clone(), split_cutoffs: splits.cutoffs,
        split_sizes: [splits.train.len(), splits.tuning.len(), splits.calibration.len(), splits.test.len()],
        lead_bins: BINS, history_modes: modes,
        limitations: "Pretrained from local ERA5 states; live input is wttr current conditions. Domain shift and receipt-time timestamps need prospective validation. Intervals are calibrated on reanalysis, not yet on wttr. No meteorological superiority claim.".into(),
    };
    let manifest = Manifest {
        version: 3,
        fingerprint: config.fingerprint(),
        dataset_sha256: checksum,
        trained_at: Utc::now().timestamp(),
        normal,
        members,
        radii,
        report,
    };
    history::save(&directory.join("manifest.json"), &manifest)?;
    history::save(&config.state_dir.join("wttr-report.json"), &manifest.report)?;
    history::save(
        &config.state_dir.join("active-wttr-model.json"),
        &generation,
    )?;
    println!("{}", serde_json::to_string_pretty(&manifest.report)?);
    Ok(())
}

pub struct Forecaster {
    pub(crate) manifest: Manifest,
    pub(crate) networks: Vec<Network>,
    pub(crate) generation: String,
}
impl Forecaster {
    pub fn load(config: &Config) -> Result<Self> {
        let generation: String = serde_json::from_slice(
            &fs::read(config.state_dir.join("active-wttr-model.json"))
                .context("wttr model is not trained; run train first")?,
        )?;
        ensure!(
            !generation.is_empty()
                && generation.len() < 64
                && generation.chars().all(|c| c.is_ascii_digit() || c == '-'),
            "invalid model generation"
        );
        let directory = config.state_dir.join("wttr-models").join(&generation);
        let manifest: Manifest =
            serde_json::from_slice(&fs::read(directory.join("manifest.json"))?)?;
        ensure!(
            manifest.version == 3 && manifest.fingerprint == config.fingerprint(),
            "model location/schema mismatch"
        );
        ensure!(
            manifest.members.len() == 3
                && manifest
                    .radii
                    .iter()
                    .flatten()
                    .flatten()
                    .all(|v| v.is_finite() && *v >= 0.0),
            "invalid ensemble/calibration"
        );
        let mut networks = vec![];
        for member in &manifest.members {
            ensure!(
                Path::new(&member.filename).components().count() == 1,
                "invalid member filename"
            );
            let path = directory.join(&member.filename);
            ensure!(
                history::digest(&fs::read(&path)?) == member.sha256,
                "model checksum mismatch"
            );
            networks.push(Network::load(&path)?);
        }
        Ok(Self {
            manifest,
            networks,
            generation,
        })
    }
    pub fn predict(&self, input: &Input) -> Result<Vec<[f32; TARGETS]>> {
        predict(&self.networks, &self.manifest.normal, input)
    }
    pub fn output(&self, input: &Input, now: i64, live: bool) -> Result<serde_json::Value> {
        if live {
            ensure!(
                (0..=6 * 3600).contains(&(now - input.origin)),
                "wttr input is stale"
            );
        } else {
            ensure!(
                input.origin >= self.manifest.report.split_cutoffs[2],
                "hindcast is restricted to the held-out period"
            );
        }
        let values = self.predict(input)?;
        ensure!(
            values
                .iter()
                .all(|v| (-90.0..=65.0).contains(&v[0]) && (850.0..=1100.0).contains(&v[1])),
            "forecast outside physical bounds"
        );
        let mode = input.mode();
        let hourly: Vec<_> = values
            .iter()
            .enumerate()
            .filter_map(|(i, v)| {
                let time = input.origin + (i + 1) as i64 * 3600;
                if live && time <= now {
                    return None;
                }
                let radius = self.manifest.radii[mode][bin(i + 1)];
                Some(
                    serde_json::json!({"valid_at":time,"lead_hours":i+1,"temperature_c":v[0],
                "pressure_hpa":v[1],"pressure_mmhg":v[1]*0.7500617,
                "temperature_interval90":[v[0]-radius[0],v[0]+radius[0]],
                "pressure_interval90_hpa":[v[1]-radius[1],v[1]+radius[1]]}),
                )
            })
            .take(if live { 48 } else { HORIZON })
            .collect();
        ensure!(!live || hourly.len() == 48, "incomplete live forecast");
        Ok(
            serde_json::json!({"generated_at":now,"forecast_type":"wttr_local_tcn","research_model":true,
            "model_generation":self.generation,"initial_state_at":input.origin,"input_source":if live {"wttr.in current conditions"} else {"ERA5 hindcast"},
            "input_points":input.points(),"calibration_history_points":HISTORY_MODES[mode],"future_forecast_inputs":false,
            "trained_at":self.manifest.trained_at,"device":"cpu","hourly":hourly}),
        )
    }
}

pub fn hindcast(config: &Config, origin: i64) -> Result<()> {
    let (data, _) = history::load(config)?;
    let index = data
        .times
        .binary_search(&origin)
        .map_err(|_| anyhow::anyhow!("origin not present"))?;
    let result = Forecaster::load(config)?.output(
        &Input::historical(&data, index, HISTORY)?,
        Utc::now().timestamp(),
        false,
    )?;
    history::save(&config.state_dir.join("wttr-hindcast.json"), &result)?;
    println!("{}", serde_json::to_string_pretty(&result)?);
    Ok(())
}

pub async fn refresh(config: &Config) -> Result<serde_json::Value> {
    let current = wttr::fetch(config).await?;
    let now = Utc::now().timestamp();
    let observations = wttr::observations(config, now - 75 * 3600)?;
    let input = Input::from_observations(&observations, now)?;
    history::save(&config.state_dir.join("wttr-input.json"), &input)?;
    let mut result = Forecaster::load(config)?.output(&input, now, true)?;
    result["current"] = serde_json::to_value(current)?;
    result["site_key"] =
        serde_json::json!(wttr::directory(config).file_name().and_then(|s| s.to_str()));
    let digest = history::digest(&serde_json::to_vec(&input)?);
    let archive = wttr::directory(config).join("forecasts").join(format!(
        "{}-{}-{}.json",
        input.origin,
        result["model_generation"].as_str().unwrap(),
        &digest[..12]
    ));
    if !archive.exists() {
        history::save(&archive, &result)?;
    }
    history::save(&config.state_dir.join("wttr-forecast.json"), &result)?;
    Ok(result)
}

pub async fn forecast(config: &Config) -> Result<()> {
    println!("{}", serde_json::to_string_pretty(&refresh(config).await?)?);
    Ok(())
}

pub fn benchmark(config: &Config, input_path: &Path, iterations: usize) -> Result<()> {
    ensure!(
        (10..=10_000).contains(&iterations),
        "iterations must be 10..10000"
    );
    let input: Input = serde_json::from_slice(&fs::read(input_path)?)?;
    input.validate()?;
    let start = Instant::now();
    let model = Forecaster::load(config)?;
    let load_ms = start.elapsed().as_secs_f64() * 1000.0;
    for _ in 0..10 {
        std::hint::black_box(model.predict(&input)?);
    }
    let mut elapsed = Vec::with_capacity(iterations);
    for _ in 0..iterations {
        let start = Instant::now();
        std::hint::black_box(model.predict(&input)?);
        elapsed.push(start.elapsed().as_secs_f64() * 1000.0);
    }
    elapsed.sort_by(f64::total_cmp);
    let q = |p: f64| elapsed[(iterations as f64 * p).ceil() as usize - 1];
    let prediction = model.predict(&input)?;
    println!(
        "{}",
        serde_json::to_string_pretty(
            &serde_json::json!({"device":"cpu","threads":candle_core::utils::get_num_threads(),
        "gpu_enabled":candle_core::utils::cuda_is_available() || candle_core::utils::metal_is_available(),
        "ensemble_members":3,"input_points":input.points(),"iterations":iterations,"model_load_ms":load_ms,
        "first_prediction":prediction[0],"prediction_sha256":history::digest(&serde_json::to_vec(&prediction)?),
        "inference_ms":{"median":q(0.5),"p95":q(0.95),"max":elapsed[iterations-1]}})
        )?
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn purges_every_future_target_at_partition_boundaries() {
        let count = 3 * 365 * 24;
        let data = Dataset {
            fingerprint: "test".into(),
            source: "test".into(),
            times: (0..count)
                .map(|i| 1_704_067_200 + i as i64 * 3600)
                .collect(),
            values: vec![0.0; count * history::CHANNELS],
            excluded_hours: 0,
        };
        let s = split(&data).unwrap();
        for (a, b) in [
            (&s.train, &s.tuning),
            (&s.tuning, &s.calibration),
            (&s.calibration, &s.test),
        ] {
            assert!(data.times[*a.last().unwrap() + HORIZON] < data.times[*b.first().unwrap()]);
        }
    }
}
