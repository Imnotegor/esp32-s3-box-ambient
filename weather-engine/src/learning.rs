use crate::{
    history::{self, Config, HISTORY, HORIZON, TARGETS},
    network::{Network, Normalization, INPUT_CHANNELS},
    training::{self, Evaluation, Forecaster, Metric, ModeReport, Report, BINS},
    wttr::{self, Input, Observation, HISTORY_MODES},
};
use anyhow::{ensure, Result};
use candle_core::{Device, Tensor};
use candle_nn::{Optimizer, VarMap};
use chrono::Utc;
use rand::{rngs::StdRng, seq::SliceRandom, SeedableRng};
use serde_json::json;
use std::{fs, path::Path};

struct Sample {
    issued: i64,
    input: Input,
    targets: Vec<Option<[f32; TARGETS]>>,
}

fn samples(observations: &[Observation]) -> Result<Vec<Sample>> {
    let mut samples = vec![];
    let mut last = i64::MIN / 2;
    for observation in observations {
        if observation.received_at - last < 6 * 3600 {
            continue;
        }
        let input = Input::from_observations(observations, observation.received_at)?;
        let mut targets = vec![None; HORIZON];
        let mut distances = [i64::MAX; HORIZON];
        for later in observations {
            if later.at <= observation.received_at || later.received_at <= observation.received_at {
                continue;
            }
            let delta = later.at - input.origin;
            let h = (delta + 1800) / 3600;
            if !(1..=HORIZON as i64).contains(&h) {
                continue;
            }
            let distance = (delta - h * 3600).abs();
            let index = h as usize - 1;
            if distance <= 1800 && distance < distances[index] {
                targets[index] = Some([later.values[0], later.values[1]]);
                distances[index] = distance;
            }
        }
        if targets.iter().flatten().count() >= 12 && targets[47..].iter().any(Option::is_some) {
            last = observation.received_at;
            samples.push(Sample {
                issued: observation.received_at,
                input,
                targets,
            });
        }
    }
    Ok(samples)
}

fn partition(samples: &[Sample]) -> Result<([Vec<usize>; 4], [i64; 3])> {
    ensure!(
        samples.len() >= 160,
        "not enough dated wttr training examples"
    );
    let first = samples[0].issued;
    let span = samples.last().unwrap().issued - first;
    let cuts = [
        first + span * 60 / 100,
        first + span * 75 / 100,
        first + span * 875 / 1000,
    ];
    let mut parts: [Vec<usize>; 4] = std::array::from_fn(|_| vec![]);
    for (i, sample) in samples.iter().enumerate() {
        let at = sample.issued;
        let end = at + HORIZON as i64 * 3600;
        if end < cuts[0] {
            parts[0].push(i);
        } else if at >= cuts[0] && end < cuts[1] {
            parts[1].push(i);
        } else if at >= cuts[1] && end < cuts[2] {
            parts[2].push(i);
        } else if at >= cuts[2] {
            parts[3].push(i);
        }
    }
    ensure!(
        parts[0].len() >= 80 && parts[1..].iter().all(|p| p.len() >= 20),
        "wttr time partitions are too small after purging future targets"
    );
    Ok((parts, cuts))
}

fn objective(
    net: &Network,
    normal: &Normalization,
    samples: &[Sample],
    indices: &[usize],
) -> Result<Tensor> {
    let mut inputs = vec![];
    let mut targets = vec![];
    let mut masks = vec![];
    for &i in indices {
        inputs.extend(normal.encode(&samples[i].input)?);
        for target in &samples[i].targets {
            match target {
                Some(value) => {
                    targets.extend(normal.target(*value));
                    masks.extend([1.0f32; TARGETS]);
                }
                None => {
                    targets.extend([0.0; TARGETS]);
                    masks.extend([0.0f32; TARGETS]);
                }
            }
        }
    }
    let count: f32 = masks.iter().sum();
    ensure!(count > 0.0, "no observed training targets");
    let x = Tensor::from_vec(
        inputs,
        (indices.len(), INPUT_CHANNELS, HISTORY),
        &Device::Cpu,
    )?;
    let y = Tensor::from_vec(targets, (indices.len(), HORIZON * TARGETS), &Device::Cpu)?;
    let mask = Tensor::from_vec(masks, (indices.len(), HORIZON * TARGETS), &Device::Cpu)?;
    Ok((((net.forward(&x)? - y)?.sqr()? * mask)?.sum_all()?).affine(1.0 / count as f64, 0.0)?)
}

fn validation(
    net: &Network,
    normal: &Normalization,
    samples: &[Sample],
    indices: &[usize],
) -> Result<f32> {
    let mut total = 0.0;
    for batch in indices.chunks(32) {
        total += objective(net, normal, samples, batch)?.to_scalar::<f32>()? * batch.len() as f32;
    }
    let loss = total / indices.len() as f32;
    ensure!(loss.is_finite(), "non-finite adaptation loss");
    Ok(loss)
}

fn fit(
    parent: &Path,
    output: &Path,
    normal: &Normalization,
    samples: &[Sample],
    parts: &[Vec<usize>; 4],
    epochs: usize,
    seed: u64,
) -> Result<Network> {
    let mut vars = VarMap::new();
    let net = Network::new(&vars)?;
    vars.load(parent)?;
    let mut optimizer = candle_nn::AdamW::new(
        vars.all_vars(),
        candle_nn::ParamsAdamW {
            lr: 0.0001,
            weight_decay: 0.0001,
            ..Default::default()
        },
    )?;
    let mut rng = StdRng::seed_from_u64(seed);
    let mut indices = parts[0].clone();
    let mut best = f32::INFINITY;
    for epoch in 0..epochs {
        indices.shuffle(&mut rng);
        for batch in indices.chunks(32) {
            optimizer.backward_step(&objective(&net, normal, samples, batch)?)?;
        }
        let loss = validation(&net, normal, samples, &parts[1])?;
        if loss < best {
            best = loss;
            vars.save(output)?;
        }
        println!("adapt seed={seed} epoch={} tuning={loss:.6}", epoch + 1);
    }
    Network::load(output)
}

fn limited(input: &Input, points: usize) -> Input {
    let mut input = input.clone();
    let mut kept = 0;
    for row in input.rows.iter_mut().rev() {
        if row.is_some() {
            kept += 1;
            if kept > points {
                *row = None;
            }
        }
    }
    input
}

fn evaluate(
    model: &Forecaster,
    samples: &[Sample],
    indices: &[usize],
    points: usize,
) -> Result<(Evaluation, Vec<Vec<[f32; TARGETS]>>)> {
    let mut evaluation = Evaluation {
        neural: Metric::default(),
        persistence: Metric::default(),
        repeat_previous_day: Metric::default(),
    };
    let mut forecasts = vec![];
    for &i in indices {
        let sample = &samples[i];
        let input = limited(&sample.input, points);
        let predicted = model.predict(&input)?;
        let current = input.rows[HISTORY - 1].unwrap();
        let last = [current[0], current[1]];
        for (h, target) in sample.targets.iter().enumerate() {
            if let Some(truth) = target {
                let day = input.rows[HISTORY - 24 + h % 24]
                    .map(|r| [r[0], r[1]])
                    .unwrap_or(last);
                evaluation.neural.add(h + 1, predicted[h], *truth);
                evaluation.persistence.add(h + 1, last, *truth);
                evaluation.repeat_previous_day.add(h + 1, day, *truth);
            }
        }
        forecasts.push(predicted);
    }
    ensure!(
        evaluation.neural.count.iter().all(|n| *n > 0),
        "missing observations in a forecast lead bin"
    );
    evaluation.neural.finish();
    evaluation.persistence.finish();
    evaluation.repeat_previous_day.finish();
    Ok((evaluation, forecasts))
}

fn overall(metric: &Metric) -> [f32; TARGETS] {
    let count = metric.count.iter().sum::<usize>() as f32;
    std::array::from_fn(|j| {
        (0..BINS.len())
            .map(|b| metric.mae[b][j] * metric.count[b] as f32)
            .sum::<f32>()
            / count
    })
}

pub fn adapt(config: &Config, epochs: usize) -> Result<()> {
    ensure!(
        (1..=20).contains(&epochs),
        "adaptation epochs must be 1..20"
    );
    let now = Utc::now().timestamp();
    let observations = wttr::observations(config, now - 180 * 86400)?;
    ensure!(
        observations.len() >= 2
            && observations.last().unwrap().at - observations[0].at >= 90 * 86400,
        "adaptation needs at least 90 days of genuine wttr history; collecting data must continue"
    );
    let through = observations.last().unwrap().at;
    let prior_path = wttr::directory(config).join("last-adaptation.json");
    if prior_path.exists() {
        let prior: serde_json::Value = serde_json::from_slice(&fs::read(&prior_path)?)?;
        ensure!(
            prior["through"]
                .as_i64()
                .is_some_and(|at| through - at >= 7 * 86400),
            "wait for at least seven more days of source history before another adaptation"
        );
    }
    let samples = samples(&observations)?;
    let (parts, cuts) = partition(&samples)?;
    let parent = Forecaster::load(config)?;
    let generation = format!("{}-{}", now, std::process::id());
    let directory = config.state_dir.join("wttr-models").join(&generation);
    fs::create_dir_all(&directory)?;
    let parent_dir = config
        .state_dir
        .join("wttr-models")
        .join(&parent.generation);
    let mut candidate = Forecaster {
        manifest: parent.manifest.clone(),
        networks: vec![],
        generation: generation.clone(),
    };
    for member in &mut candidate.manifest.members {
        let path = directory.join(&member.filename);
        candidate.networks.push(fit(
            &parent_dir.join(&member.filename),
            &path,
            &parent.manifest.normal,
            &samples,
            &parts,
            epochs,
            member.seed,
        )?);
        member.sha256 = history::digest(&fs::read(path)?);
    }
    let mut modes = vec![];
    for (mode, points) in HISTORY_MODES.into_iter().enumerate() {
        let (tuning, _) = evaluate(&candidate, &samples, &parts[1], points)?;
        let (_, calibrated) = evaluate(&candidate, &samples, &parts[2], points)?;
        let mut errors: Vec<Vec<Vec<f32>>> = (0..5).map(|_| vec![vec![], vec![]]).collect();
        for (&i, predicted) in parts[2].iter().zip(&calibrated) {
            for (h, target) in samples[i].targets.iter().enumerate() {
                if let Some(truth) = target {
                    for (j, value) in truth.iter().enumerate() {
                        errors[training::bin(h + 1)][j].push((predicted[h][j] - value).abs());
                    }
                }
            }
        }
        for (b, row) in errors.iter_mut().enumerate() {
            for (j, values) in row.iter_mut().enumerate() {
                ensure!(
                    values.len() >= 20,
                    "not enough actual observations for interval calibration"
                );
                values.sort_by(f32::total_cmp);
                let rank =
                    (((values.len() + 1) as f32 * 0.9).ceil() as usize).min(values.len()) - 1;
                candidate.manifest.radii[mode][b][j] = values[rank];
            }
        }
        let (holdout, predicted) = evaluate(&candidate, &samples, &parts[3], points)?;
        let mut coverage = [[0.0; TARGETS]; 5];
        for (&i, predicted) in parts[3].iter().zip(&predicted) {
            for (h, target) in samples[i].targets.iter().enumerate() {
                if let Some(truth) = target {
                    let b = training::bin(h + 1);
                    for (j, value) in truth.iter().enumerate() {
                        if (predicted[h][j] - value).abs() <= candidate.manifest.radii[mode][b][j] {
                            coverage[b][j] += 1.0;
                        }
                    }
                }
            }
        }
        for (b, row) in coverage.iter_mut().enumerate() {
            for value in row {
                *value /= holdout.neural.count[b] as f32;
            }
        }
        modes.push(ModeReport {
            history_points: points,
            tuning,
            holdout,
            coverage90: coverage,
        });
    }
    let (base, _) = evaluate(&parent, &samples, &parts[3], HISTORY)?;
    let old = overall(&base.neural);
    let new = overall(&modes.last().unwrap().holdout.neural);
    let accepted = (0..TARGETS).all(|j| new[j] <= old[j] * 0.98);
    candidate.manifest.dataset_sha256 = history::digest(&serde_json::to_vec(&observations)?);
    candidate.manifest.trained_at = now;
    candidate.manifest.report=Report{input_contract:parent.manifest.report.input_contract.clone(),source:"wttr current conditions, with receipt timestamps when source UTC timestamps are absent".into(),
        split_cutoffs:cuts,split_sizes:std::array::from_fn(|i|parts[i].len()),lead_bins:BINS,history_modes:modes,
        limitations:"Rolling 180-day source archive, 90-day minimum. Future targets crossing partitions are purged. Labels are subsequent wttr values within 30 minutes, not independent station truth. Missing targets do not contribute to loss. Previous-day baseline falls back to persistence when that past observation is missing. Activation requires at least 2% MAE improvement for both variables; this is not a guarantee of future improvement.".into()};
    let decision = json!({"accepted":accepted,"parent_generation":parent.generation,"candidate_generation":generation,"through":through,"parent_mae":old,"candidate_mae":new});
    history::save(&directory.join("manifest.json"), &candidate.manifest)?;
    history::save(&directory.join("adaptation.json"), &decision)?;
    if accepted {
        history::save(
            &config.state_dir.join("wttr-report.json"),
            &candidate.manifest.report,
        )?;
        history::save(
            &config.state_dir.join("active-wttr-model.json"),
            &generation,
        )?;
    }
    history::save(&prior_path, &decision)?;
    println!("{}", serde_json::to_string_pretty(&decision)?);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn observations() -> Vec<Observation> {
        (0..110 * 24)
            .map(|i| Observation {
                at: 1_700_000_000 + i * 3600,
                received_at: 1_700_000_000 + i * 3600,
                timestamp_basis: "first_received".into(),
                source_clock: String::new(),
                values: [10.0 + (i as f32 * 0.03).sin(), 1015.0, 65.0, 1.0, 2.0, 50.0],
                fingerprint: String::new(),
            })
            .collect()
    }
    #[test]
    fn observed_training_purges_future_targets_and_does_not_leak_future_inputs() -> Result<()> {
        let mut observations = observations();
        let first = samples(&observations)?;
        let (parts, _) = partition(&first)?;
        for pair in parts.windows(2) {
            assert!(
                first[*pair[0].last().unwrap()].issued + HORIZON as i64 * 3600
                    < first[pair[1][0]].issued
            );
        }
        let at = first[20].issued;
        for row in observations.iter_mut().filter(|o| o.at > at) {
            row.values[0] += 5.0;
        }
        let changed = samples(&observations)?;
        assert_eq!(first[20].input.rows, changed[20].input.rows);
        assert_ne!(first[20].targets, changed[20].targets);
        Ok(())
    }
    #[test]
    fn missing_observed_targets_are_masked_during_cpu_training() -> Result<()> {
        let mut samples = samples(&observations())?;
        samples[0].targets[2] = None;
        samples[0].targets[10] = None;
        let normal = Normalization {
            mean: vec![0.0; 6],
            scale: vec![1000.0; 6],
        };
        let vars = VarMap::new();
        let net = Network::new(&vars)?;
        crate::network::initialize(&vars, 42)?;
        let loss = objective(&net, &normal, &samples, &[0, 1])?;
        assert!(loss.to_scalar::<f32>()?.is_finite());
        let mut optimizer = candle_nn::AdamW::new_lr(vars.all_vars(), 0.0001)?;
        optimizer.backward_step(&loss)?;
        Ok(())
    }
}
