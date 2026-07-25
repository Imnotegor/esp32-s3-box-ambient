use core::fmt;

use esp_idf_sys::box3_bridge as ffi;

#[derive(Debug)]
pub struct SpeakerError(i32);

impl fmt::Display for SpeakerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "BOX-3 speaker error {}", self.0)
    }
}

pub struct Speaker;

impl Speaker {
    pub fn open(sample_rate: u32, channels: u8, bits_per_sample: u8) -> Result<Self, SpeakerError> {
        check(unsafe { ffi::box3_bridge_speaker_open(sample_rate, channels, bits_per_sample) })?;
        Ok(Self)
    }

    pub fn set_volume(&mut self, volume_percent: u8) -> Result<(), SpeakerError> {
        check(unsafe { ffi::box3_bridge_speaker_set_volume(volume_percent.into()) })
    }

    pub fn write(&mut self, samples: &[i16]) -> Result<(), SpeakerError> {
        check(unsafe { ffi::box3_bridge_speaker_write(samples.as_ptr(), samples.len()) })
    }
}

impl Drop for Speaker {
    fn drop(&mut self) {
        let _ = unsafe { ffi::box3_bridge_speaker_close() };
    }
}

fn check(result: i32) -> Result<(), SpeakerError> {
    if result == 0 {
        Ok(())
    } else {
        Err(SpeakerError(result))
    }
}
