mod board;

use core::f32::consts::TAU;

use board::Speaker;

fn main() {
    esp_idf_sys::link_patches();

    println!("starting BOX-3 speaker smoke test");

    if let Err(error) = play_test_tone() {
        eprintln!("speaker smoke test failed: {error}");
        return;
    }

    println!("speaker smoke test completed");
}

fn play_test_tone() -> Result<(), board::SpeakerError> {
    const SAMPLE_RATE: u32 = 48_000;
    const BLOCK_SAMPLES: usize = 480;
    const BLOCKS: usize = 200;
    const FREQUENCY: f32 = 440.0;
    const AMPLITUDE: f32 = 0.1;

    let mut speaker = Speaker::open(SAMPLE_RATE, 1, 16)?;
    speaker.set_volume(10)?;

    let phase_step = TAU * FREQUENCY / SAMPLE_RATE as f32;
    let mut phase = 0.0_f32;
    let mut block = [0_i16; BLOCK_SAMPLES];

    for _ in 0..BLOCKS {
        for sample in &mut block {
            *sample = (phase.sin() * AMPLITUDE * i16::MAX as f32) as i16;
            phase = (phase + phase_step) % TAU;
        }
        speaker.write(&block)?;
    }

    block.fill(0);
    speaker.write(&block)
}
