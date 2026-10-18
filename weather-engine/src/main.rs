#![forbid(unsafe_code)]

mod box_forecast;
mod history;
mod learning;
mod network;
mod service;
mod training;
mod wttr;

use anyhow::Result;
use chrono::NaiveDate;
use clap::{Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser)]
struct Arguments {
    #[arg(long)]
    config: PathBuf,
    /// Maximum number of CPU compute threads used by Candle and Rayon.
    #[arg(long, global = true, default_value_t = 2, value_parser = clap::value_parser!(u16).range(1..=32))]
    threads: u16,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Collect {
        #[arg(long)]
        start: NaiveDate,
        #[arg(long)]
        end: NaiveDate,
    },
    Train {
        #[arg(long, default_value_t = 40)]
        epochs: usize,
    },
    /// Adapt the existing model after enough genuine wttr history has accumulated.
    Adapt {
        #[arg(long, default_value_t = 10)]
        epochs: usize,
    },
    Hindcast {
        #[arg(long)]
        origin: i64,
    },
    Forecast,
    Serve {
        #[arg(long, default_value = "127.0.0.1:8790")]
        bind: std::net::SocketAddr,
        #[arg(long, default_value_t = 600)]
        refresh_seconds: u64,
    },
    ImportWttr {
        #[arg(long)]
        path: PathBuf,
    },
    /// Time the complete ensemble offline using a saved atmospheric input.
    Benchmark {
        #[arg(long)]
        input: PathBuf,
        #[arg(long, default_value_t = 200)]
        iterations: usize,
    },
    Report,
}

fn main() -> Result<()> {
    let args = Arguments::parse();
    // Candle and Rayon read this setting; set it before starting any threads.
    std::env::set_var("RAYON_NUM_THREADS", args.threads.to_string());
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()?
        .block_on(run(args))
}

async fn run(args: Arguments) -> Result<()> {
    let config = history::Config::load(&args.config)?;
    match args.command {
        Command::Collect { start, end } => history::collect(&config, start, end).await?,
        Command::Train { epochs } => training::train(&config, epochs)?,
        Command::Adapt { epochs } => learning::adapt(&config, epochs)?,
        Command::Hindcast { origin } => training::hindcast(&config, origin)?,
        Command::Forecast => training::forecast(&config).await?,
        Command::Serve {
            bind,
            refresh_seconds,
        } => service::serve(config, bind, refresh_seconds).await?,
        Command::ImportWttr { path } => {
            let raw: serde_json::Value = serde_json::from_slice(&std::fs::read(path)?)?;
            let received = raw["received_at"]
                .as_i64()
                .ok_or_else(|| anyhow::anyhow!("missing received_at"))?;
            wttr::ingest(&config, &raw["response"], received)?;
            println!("Observation imported");
        }
        Command::Benchmark { input, iterations } => {
            training::benchmark(&config, &input, iterations)?;
        }
        Command::Report => println!(
            "{}",
            std::fs::read_to_string(config.state_dir.join("wttr-report.json"))?
        ),
    }
    Ok(())
}
