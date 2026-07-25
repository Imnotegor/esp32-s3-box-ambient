#include "box3_bridge.h"

#include <stdbool.h>

#include "bsp/esp-box-3.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

static esp_codec_dev_handle_t speaker;
static bool speaker_open;

int32_t box3_bridge_speaker_open(uint32_t sample_rate, uint8_t channels,
                                 uint8_t bits_per_sample) {
    if (speaker_open) {
        return ESP_ERR_INVALID_STATE;
    }

    if (speaker == NULL) {
        speaker = bsp_audio_codec_speaker_init();
        if (speaker == NULL) {
            return ESP_FAIL;
        }
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = sample_rate,
        .channel = channels,
        .bits_per_sample = bits_per_sample,
    };
    int result = esp_codec_dev_open(speaker, &format);
    if (result == ESP_OK) {
        speaker_open = true;
    }
    return result;
}

int32_t box3_bridge_speaker_set_volume(int32_t volume_percent) {
    if (speaker == NULL || volume_percent < 0 || volume_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_codec_dev_set_out_vol(speaker, volume_percent);
}

int32_t box3_bridge_speaker_write(const int16_t *samples,
                                  size_t sample_count) {
    if (!speaker_open || samples == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_write(speaker, samples,
                               sample_count * sizeof(*samples));
}

int32_t box3_bridge_speaker_close(void) {
    if (!speaker_open) {
        return ESP_ERR_INVALID_STATE;
    }
    int result = esp_codec_dev_close(speaker);
    if (result == ESP_OK) {
        speaker_open = false;
    }
    return result;
}
