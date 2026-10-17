/*
 * Fixed 16 kHz stereo bring-up. Codec register sequences adapted from
 * Espressif esp-adf/components/esp_codec_dev/device/{es8311,es7210}
 * (Apache-2.0, Copyright Espressif Systems).
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/device.h>
#include "diagnostics.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/shell/shell.h>
#include "audio.h"

#define BLOCK_BYTES 1024
#define FRAMES (BLOCK_BYTES / 4)
#define ES8311 0x18
#define ES7210 0x40

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const audio = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const pa_gpio = DEVICE_DT_GET(DT_NODELABEL(gpio1));
K_MEM_SLAB_DEFINE_STATIC(audio_slab, BLOCK_BYTES, 12, 4);
static K_SEM_DEFINE(beep_request, 0, 1);
static atomic_t audio_running;
static atomic_t mic_peak_l;
static atomic_t mic_peak_r;

void audio_summary(char *buf, size_t len)
{
    snprintf(buf, len, "Audio: %s | mic: %ld/%ld",
             atomic_get(&audio_running) ? "ON" : "OFF",
             (long)atomic_get(&mic_peak_l), (long)atomic_get(&mic_peak_r));
}

struct reg_value { uint8_t reg; uint8_t value; };

static int write_regs(uint16_t address, const struct reg_value *regs, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        int ret = i2c_reg_write_byte(bus, address, regs[i].reg, regs[i].value);

        /* Espressif documents a transient NACK when setting ES8311 noise immunity. */
        if (ret < 0 && address == ES8311 && regs[i].reg == 0x44) {
            ret = i2c_reg_write_byte(bus, address, regs[i].reg, regs[i].value);
        }
        if (ret < 0) {
            printk("BOX3_AUDIO codec=0x%x reg=0x%x error=%d\n",
                   address, regs[i].reg, ret);
            return ret;
        }
    }
    return 0;
}

static int codec_init(void)
{
    /* ES8311 slave, 16-bit I2S, MCLK=256*16 kHz; muted until DMA starts. */
    static const struct reg_value dac[] = {
        {0x0d,0xfa}, {0x44,0x08}, {0x44,0x08},
        {0x01,0x30}, {0x02,0x00}, {0x03,0x10}, {0x16,0x24},
        {0x04,0x10}, {0x05,0x00}, {0x0b,0x00}, {0x0c,0x00},
        {0x10,0x1f}, {0x11,0x7f}, {0x00,0x80}, {0x01,0x3f},
        {0x06,0x03}, {0x13,0x10}, {0x1b,0x0a}, {0x1c,0x6a},
        {0x44,0x58}, {0x09,0x0c}, {0x0a,0x0c}, {0x07,0x00},
        {0x08,0xff}, {0x04,0x20}, {0x17,0xbf}, {0x0e,0x02},
        {0x12,0x00}, {0x14,0x1a}, {0x0d,0x01}, {0x15,0x40},
        {0x37,0x08}, {0x45,0x00}, {0x32,0xdf}, {0x31,0x60},
    };
    /* ES7210 slave: MIC1+MIC2, 18 dB PGA, stereo 16-bit I2S. */
    static const struct reg_value adc[] = {
        {0x00,0xff}, {0x00,0x41}, {0x01,0x3f},
        {0x09,0x30}, {0x0a,0x30}, {0x23,0x2a}, {0x22,0x0a},
        {0x20,0x0a}, {0x21,0x2a}, {0x08,0x00},
        {0x40,0x43}, {0x41,0x70}, {0x42,0x70},
        {0x07,0x20}, {0x02,0xc1}, {0x11,0x60}, {0x12,0x00},
        {0x43,0x16}, {0x44,0x16}, {0x45,0x00}, {0x46,0x00},
        {0x4b,0x00}, {0x4c,0xff}, {0x01,0x34},
        {0x06,0x00}, {0x47,0x08}, {0x48,0x08}, {0x49,0x08},
        {0x4a,0x08}, {0x40,0x43}, {0x00,0x71}, {0x00,0x41},
    };
    int ret = write_regs(ES8311, dac, ARRAY_SIZE(dac));

    if (ret == 0) {
        ret = write_regs(ES7210, adc, ARRAY_SIZE(adc));
    }
    return ret;
}

void audio_beep(void)
{
    k_sem_give(&beep_request);
}

static int cmd_beep(const struct shell *shell, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    if (!atomic_get(&audio_running)) {
        shell_error(shell, "Audio is not running");
        return -ENODEV;
    }
    audio_beep();
    shell_print(shell, "Queued quiet 500 Hz / 300 ms tone");
    return 0;
}

SHELL_CMD_ARG_REGISTER(box3_beep, NULL, "Play a quiet test tone", cmd_beep, 1, 0);

static void audio_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    int ret;
    uint32_t tone_frames = 0;
    uint32_t phase = 0;
    uint32_t total_frames = 0;
    uint32_t peak[2] = {0};
    uint64_t sum[2] = {0};
    int64_t next_report = k_uptime_get() + 2000;
    static const int16_t sine[32] = {
        0,780,1531,2222,2828,3326,3696,3923,
        4000,3923,3696,3326,2828,2222,1531,780,
        0,-780,-1531,-2222,-2828,-3326,-3696,-3923,
        -4000,-3923,-3696,-3326,-2828,-2222,-1531,-780,
    };
    if (!device_is_ready(bus) || !device_is_ready(audio) || !device_is_ready(pa_gpio)) {
        printk("BOX3_AUDIO devices_not_ready\n");
        return;
    }
    ret = gpio_pin_configure(pa_gpio, 14, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("BOX3_AUDIO pa_config=%d\n", ret);
        return;
    }
    ret = codec_init();
    printk("BOX3_AUDIO codec_init=%d\n", ret);
    if (ret < 0) {
        return;
    }

    struct i2s_config cfg = {
        .word_size = 16, .channels = 2, .format = I2S_FMT_DATA_FORMAT_I2S,
        .options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
        .frame_clk_freq = 16000, .mem_slab = &audio_slab,
        .block_size = BLOCK_BYTES, .timeout = 1000,
    };
    ret = i2s_configure(audio, I2S_DIR_BOTH, &cfg);
    printk("BOX3_AUDIO i2s_configure=%d\n", ret);
    if (ret < 0) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        void *block;

        ret = k_mem_slab_alloc(&audio_slab, &block, K_MSEC(100));
        if (ret < 0) {
            goto stop;
        }
        memset(block, 0, BLOCK_BYTES);
        ret = i2s_write(audio, block, BLOCK_BYTES);
        if (ret < 0) {
            k_mem_slab_free(&audio_slab, block);
            goto stop;
        }
    }
    ret = i2s_trigger(audio, I2S_DIR_BOTH, I2S_TRIGGER_START);
    printk("BOX3_AUDIO i2s_start=%d\n", ret);
    if (ret < 0) {
        goto stop;
    }
    ret = i2c_reg_write_byte(bus, ES8311, 0x31, 0x00);
    if (ret < 0) {
        goto stop;
    }
    ret = gpio_pin_set(pa_gpio, 14, 1);
    if (ret < 0) {
        goto stop;
    }
    atomic_set(&audio_running, 1);
    audio_beep();

    while (1) {
        void *rx;
        void *tx;
        size_t size;

        ret = i2s_read(audio, &rx, &size);
        if (ret < 0) {
            goto stop;
        }
        if (size > BLOCK_BYTES || size % 4 != 0) {
            k_mem_slab_free(&audio_slab, rx);
            ret = -EIO;
            goto stop;
        }
        int16_t *samples = rx;
        for (size_t i = 0; i < size / 2; ++i) {
            uint32_t magnitude = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];

            peak[i % 2] = MAX(peak[i % 2], magnitude);
            sum[i % 2] += magnitude;
        }
        total_frames += size / 4;
        k_mem_slab_free(&audio_slab, rx);
        if (k_sem_take(&beep_request, K_NO_WAIT) == 0) {
            tone_frames = 4800;
        }
        ret = k_mem_slab_alloc(&audio_slab, &tx, K_MSEC(100));
        if (ret < 0) {
            goto stop;
        }
        int16_t *out = tx;
        for (size_t frame = 0; frame < FRAMES; ++frame) {
            int16_t sample = tone_frames ? sine[(phase++) % 32] : 0;

            if (tone_frames) {
                --tone_frames;
            }
            out[frame * 2] = sample;
            out[frame * 2 + 1] = sample;
        }
        ret = i2s_write(audio, tx, BLOCK_BYTES);
        if (ret < 0) {
            k_mem_slab_free(&audio_slab, tx);
            goto stop;
        }
        if (k_uptime_get() >= next_report) {
            atomic_set(&mic_peak_l, peak[0]);
            atomic_set(&mic_peak_r, peak[1]);
            printk("BOX3_AUDIO frames=%u peak_L=%u peak_R=%u avg_L=%u avg_R=%u\n",
                   total_frames, peak[0], peak[1], total_frames ? (uint32_t)(sum[0] / total_frames) : 0,
                   total_frames ? (uint32_t)(sum[1] / total_frames) : 0);
            total_frames = 0;
            memset(peak, 0, sizeof(peak));
            memset(sum, 0, sizeof(sum));
            next_report = k_uptime_get() + 2000;
        }
    }

stop:
    atomic_clear(&audio_running);
    printk("BOX3_AUDIO stopped error=%d\n", ret);
    gpio_pin_set(pa_gpio, 14, 0);
    i2c_reg_write_byte(bus, ES8311, 0x31, 0x60);
    i2s_trigger(audio, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
}

K_THREAD_DEFINE(audio_probe, 3072, audio_thread, NULL, NULL, NULL, 6, 0, 2500);
