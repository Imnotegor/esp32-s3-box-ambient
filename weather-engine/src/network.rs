use crate::history::{Dataset, CHANNELS, GRID, HISTORY, HORIZON, TARGETS, VARIABLES};
use crate::wttr::{self, Input, HISTORY_MODES, WEATHER_FIELDS as FIELDS};
use anyhow::{ensure, Result};
use candle_core::{DType, Device, Tensor};
use candle_nn::{Conv1d, Conv1dConfig, Linear, Module, VarBuilder, VarMap};
use rand::{rngs::StdRng, SeedableRng};
use rand_distr::{Distribution, Normal};
use serde::{Deserialize, Serialize};
use std::path::Path;

pub const WIDTH: usize = 32;
pub const INPUT_CHANNELS: usize = FIELDS + 4 + 1;
const DILATIONS: [usize; 6] = [1, 2, 4, 8, 16, 32];

#[derive(Clone, Serialize, Deserialize)]
pub struct Normalization {
    pub mean: Vec<f32>,
    pub scale: Vec<f32>,
}
impl Normalization {
    pub fn fit(data: &Dataset, cutoff: i64) -> Result<Self> {
        let end = data.times.partition_point(|t| *t < cutoff);
        ensure!(end > HISTORY, "insufficient training history");
        let mut mean = vec![0.0; FIELDS];
        let mut scale = vec![0.0; FIELDS];
        for c in 0..FIELDS {
            let column = (GRID / 2) * VARIABLES + c;
            let m = (0..end)
                .map(|t| data.values[t * CHANNELS + column] as f64)
                .sum::<f64>()
                / end as f64;
            mean[c] = m as f32;
            scale[c] = ((0..end)
                .map(|t| (data.values[t * CHANNELS + column] as f64 - m).powi(2))
                .sum::<f64>()
                / end as f64)
                .sqrt()
                .max(0.01) as f32;
        }
        Ok(Self { mean, scale })
    }
    pub fn encode(&self, input: &Input) -> Result<Vec<f32>> {
        input.validate()?;
        ensure!(
            self.mean.len() == FIELDS && self.scale.len() == FIELDS,
            "normalization shape mismatch"
        );
        ensure!(
            self.mean.iter().all(|v| v.is_finite())
                && self.scale.iter().all(|v| v.is_finite() && *v > 0.0),
            "invalid normalization values"
        );
        let mut out = vec![0.0; INPUT_CHANNELS * HISTORY];
        for t in 0..HISTORY {
            if let Some(row) = input.rows[t] {
                for c in 0..FIELDS {
                    out[c * HISTORY + t] = (row[c] - self.mean[c]) / self.scale[c];
                }
                out[(INPUT_CHANNELS - 1) * HISTORY + t] = 1.0;
            }
            let calendar = wttr::calendar(input.origin - (HISTORY - 1 - t) as i64 * 3600)?;
            for c in 0..4 {
                out[(FIELDS + c) * HISTORY + t] = calendar[c];
            }
        }
        Ok(out)
    }
    pub fn target(&self, value: [f32; TARGETS]) -> [f32; TARGETS] {
        std::array::from_fn(|j| (value[j] - self.mean[j]) / self.scale[j])
    }
    pub fn decode(&self, value: [f32; TARGETS]) -> [f32; TARGETS] {
        std::array::from_fn(|j| value[j] * self.scale[j] + self.mean[j])
    }
}

pub struct Network {
    convolutions: Vec<Conv1d>,
    output: Linear,
}
impl Network {
    pub fn new(vars: &VarMap) -> Result<Self> {
        let vb = VarBuilder::from_varmap(vars, DType::F32, &Device::Cpu);
        Self::from_builder(vb)
    }
    pub fn load(path: &Path) -> Result<Self> {
        let weights = candle_core::safetensors::load(path, &Device::Cpu)?;
        Self::from_builder(VarBuilder::from_tensors(weights, DType::F32, &Device::Cpu))
    }
    fn from_builder(vb: VarBuilder<'_>) -> Result<Self> {
        let mut convolutions = vec![];
        for (i, &dilation) in DILATIONS.iter().enumerate() {
            let channels = if i == 0 { INPUT_CHANNELS } else { WIDTH };
            let config = Conv1dConfig {
                dilation,
                ..Default::default()
            };
            convolutions.push(candle_nn::conv1d(
                channels,
                WIDTH,
                3,
                config,
                vb.pp(format!("causal_{i}")),
            )?);
        }
        Ok(Self {
            convolutions,
            output: candle_nn::linear(WIDTH, HORIZON * TARGETS, vb.pp("future"))?,
        })
    }
    pub fn forward(&self, history: &Tensor) -> candle_core::Result<Tensor> {
        let mut state = history.clone();
        for (i, (layer, &dilation)) in self.convolutions.iter().zip(&DILATIONS).enumerate() {
            // The output head only consumes the final time step. The final
            // convolution needs its three taps, not 72 discarded outputs.
            let (input, residual) = if i + 1 == DILATIONS.len() {
                (
                    state.narrow(2, HISTORY - 1 - 2 * dilation, 2 * dilation + 1)?,
                    state.narrow(2, HISTORY - 1, 1)?,
                )
            } else {
                (state.pad_with_zeros(2, 2 * dilation, 0)?, state.clone())
            };
            let next = layer.forward(&input)?.relu()?;
            state = if i == 0 {
                next
            } else {
                (residual + next)?.affine(std::f64::consts::FRAC_1_SQRT_2, 0.0)?
            };
        }
        let change = self.output.forward(&state.squeeze(2)?)?;
        let current = history
            .narrow(1, 0, TARGETS)?
            .narrow(2, HISTORY - 1, 1)?
            .squeeze(2)?
            .unsqueeze(1)?
            .broadcast_as((history.dim(0)?, HORIZON, TARGETS))?
            .flatten_from(1)?;
        change + current
    }
}

pub fn initialize(vars: &VarMap, seed: u64) -> Result<()> {
    let mut rng = StdRng::seed_from_u64(seed);
    let map = vars
        .data()
        .lock()
        .map_err(|_| anyhow::anyhow!("poisoned model lock"))?;
    let mut names: Vec<_> = map.keys().collect();
    names.sort();
    for name in names {
        let variable = &map[name];
        let values = if name.ends_with("bias") || name == "future.weight" {
            vec![0.0; variable.elem_count()]
        } else {
            let fan_in: usize = variable.dims()[1..].iter().product();
            let distribution = Normal::new(0.0f32, (2.0 / fan_in as f32).sqrt())?;
            (0..variable.elem_count())
                .map(|_| distribution.sample(&mut rng))
                .collect()
        };
        variable.set(&Tensor::from_vec(values, variable.shape(), &Device::Cpu)?)?;
    }
    Ok(())
}

pub fn batch(
    data: &Dataset,
    indices: &[usize],
    normal: &Normalization,
    mask_seed: usize,
) -> Result<(Tensor, Tensor)> {
    let mut inputs = Vec::with_capacity(indices.len() * INPUT_CHANNELS * HISTORY);
    let mut targets = Vec::with_capacity(indices.len() * HORIZON * TARGETS);
    for &origin in indices {
        let mut input = Input::historical(
            data,
            origin,
            HISTORY_MODES[(origin / 6 + mask_seed) % HISTORY_MODES.len()],
        )?;
        for t in 0..HISTORY - 1 {
            if (origin + t * 37 + mask_seed * 11).is_multiple_of(17) {
                input.rows[t] = None;
            }
        }
        inputs.extend(normal.encode(&input)?);
        ensure!(
            origin + HORIZON < data.times.len()
                && data.times[origin + HORIZON] - data.times[origin] == HORIZON as i64 * 3600,
            "future target gap"
        );
        for h in 1..=HORIZON {
            targets.extend(normal.target(data.target(origin + h)));
        }
    }
    Ok((
        Tensor::from_vec(
            inputs,
            (indices.len(), INPUT_CHANNELS, HISTORY),
            &Device::Cpu,
        )?,
        Tensor::from_vec(targets, (indices.len(), HORIZON * TARGETS), &Device::Cpu)?,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use candle_nn::Optimizer;

    #[test]
    fn final_step_optimization_preserves_outputs_and_training_gradients() -> Result<()> {
        let variables = VarMap::new();
        let network = Network::new(&variables)?;
        initialize(&variables, 42)?;
        {
            let map = variables.data().lock().unwrap();
            let head = &map["future.weight"];
            let values: Vec<_> = (0..head.elem_count())
                .map(|i| (i as f32 * 0.031).sin() * 0.03)
                .collect();
            head.set(&Tensor::from_vec(values, head.shape(), &Device::Cpu)?)?;
        }
        let values: Vec<_> = (0..2 * INPUT_CHANNELS * HISTORY)
            .map(|i| (i as f32 * 0.013).sin())
            .collect();
        let input = Tensor::from_vec(values, (2, INPUT_CHANNELS, HISTORY), &Device::Cpu)?;
        let optimized = network.forward(&input)?;

        // Original full-window implementation is the numerical reference.
        let mut state = input.clone();
        for (i, (layer, &dilation)) in network.convolutions.iter().zip(&DILATIONS).enumerate() {
            let next = layer
                .forward(&state.pad_with_zeros(2, 2 * dilation, 0)?)?
                .relu()?;
            state = if i == 0 {
                next
            } else {
                (state + next)?.affine(std::f64::consts::FRAC_1_SQRT_2, 0.0)?
            };
        }
        let change = network
            .output
            .forward(&state.narrow(2, HISTORY - 1, 1)?.squeeze(2)?)?;
        let current = input
            .narrow(1, 0, TARGETS)?
            .narrow(2, HISTORY - 1, 1)?
            .squeeze(2)?
            .unsqueeze(1)?
            .broadcast_as((2, HORIZON, TARGETS))?
            .flatten_from(1)?;
        let reference = (change + current)?;
        let difference = (&optimized - &reference)?
            .abs()?
            .max_all()?
            .to_scalar::<f32>()?;
        assert!(difference < 0.00002, "output difference {difference}");

        let optimized_gradients = optimized.sum_all()?.backward()?;
        let reference_gradients = reference.sum_all()?.backward()?;
        for (name, variable) in variables.data().lock().unwrap().iter() {
            let delta = (optimized_gradients.get(variable).unwrap()
                - reference_gradients.get(variable).unwrap())?
            .abs()?
            .max_all()?
            .to_scalar::<f32>()?;
            assert!(delta < 0.0005, "gradient difference for {name}: {delta}");
        }
        Ok(())
    }

    #[test]
    fn inference_weights_do_not_build_an_autograd_graph() -> Result<()> {
        let variables = VarMap::new();
        Network::new(&variables)?;
        initialize(&variables, 42)?;
        let weights = variables
            .data()
            .lock()
            .unwrap()
            .iter()
            .map(|(name, variable)| (name.clone(), variable.detach()))
            .collect();
        let network =
            Network::from_builder(VarBuilder::from_tensors(weights, DType::F32, &Device::Cpu))?;
        let input = Tensor::zeros((1, INPUT_CHANNELS, HISTORY), DType::F32, &Device::Cpu)?;
        assert!(!network.forward(&input)?.track_op());
        Ok(())
    }

    #[test]
    fn convolutional_model_really_learns_with_cpu_backpropagation() {
        let variables = VarMap::new();
        let network = Network::new(&variables).unwrap();
        initialize(&variables, 42).unwrap();
        let input: Vec<_> = (0..2 * INPUT_CHANNELS * HISTORY)
            .map(|i| (i as f32 * 0.013).sin() * 0.1)
            .collect();
        let x = Tensor::from_vec(input, (2, INPUT_CHANNELS, HISTORY), &Device::Cpu).unwrap();
        let y = Tensor::ones((2, HORIZON * TARGETS), DType::F32, &Device::Cpu).unwrap();
        let before = candle_nn::loss::mse(&network.forward(&x).unwrap(), &y).unwrap();
        let initial = before.to_scalar::<f32>().unwrap();
        let mut optimizer = candle_nn::AdamW::new_lr(variables.all_vars(), 0.001).unwrap();
        optimizer.backward_step(&before).unwrap();
        let after = candle_nn::loss::mse(&network.forward(&x).unwrap(), &y)
            .unwrap()
            .to_scalar::<f32>()
            .unwrap();
        assert!(after.is_finite() && after < initial, "{initial} -> {after}");
    }
    #[test]
    fn changing_future_observations_cannot_change_model_input() {
        let total = HISTORY + HORIZON;
        let mut data = Dataset {
            fingerprint: "test".into(),
            source: "test".into(),
            times: (0..total).map(|i| i as i64 * 3600).collect(),
            values: vec![1.0; total * CHANNELS],
            excluded_hours: 0,
        };
        let norm = Normalization {
            mean: vec![0.0; FIELDS],
            scale: vec![1.0; FIELDS],
        };
        let origin = HISTORY - 1;
        let first = norm
            .encode(&Input::historical(&data, origin, HISTORY).unwrap())
            .unwrap();
        data.values[HISTORY * CHANNELS..].fill(9000.0);
        assert_eq!(
            first,
            norm.encode(&Input::historical(&data, origin, HISTORY).unwrap())
                .unwrap()
        );
    }

    #[test]
    fn normalization_does_not_read_after_training_cutoff() {
        let hours = 300;
        let mut data = Dataset {
            fingerprint: "test".into(),
            source: "test".into(),
            times: (0..hours).map(|i| i as i64 * 3600).collect(),
            values: (0..hours * CHANNELS)
                .map(|i| (i % CHANNELS) as f32)
                .collect(),
            excluded_hours: 0,
        };
        let cutoff = 200 * 3600;
        let before = Normalization::fit(&data, cutoff).unwrap();
        data.values[200 * CHANNELS..].fill(10000.0);
        let after = Normalization::fit(&data, cutoff).unwrap();
        assert_eq!(before.mean, after.mean);
        assert_eq!(before.scale, after.scale);
    }
}
