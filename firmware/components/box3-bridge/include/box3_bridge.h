#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t box3_bridge_speaker_open(uint32_t sample_rate, uint8_t channels,
                                 uint8_t bits_per_sample);
int32_t box3_bridge_speaker_set_volume(int32_t volume_percent);
int32_t box3_bridge_speaker_write(const int16_t *samples,
                                  size_t sample_count);
int32_t box3_bridge_speaker_close(void);

#ifdef __cplusplus
}
#endif
