#include <errno.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/version.h>
#include <lvgl.h>
#include "dashboard.h"
#include "telemetry.h"
#include "ui.h"

#define AHT30_ADDRESS 0x38
static const struct device *const dock = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct device *const touch = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));
static atomic_t requested_page = ATOMIC_INIT(-1);
static atomic_t requested_weather = ATOMIC_INIT(-1);
static uint32_t render_started, render_count, render_total_us, render_max_us;

static void render_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        render_started = k_cycle_get_32();
    } else {
        uint32_t elapsed = k_cyc_to_us_floor32(k_cycle_get_32() - render_started);
        render_count++;
        render_total_us += elapsed;
        render_max_us = MAX(render_max_us, elapsed);
    }
}

static int page_command(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    char *end;
    unsigned long page = strtoul(argv[1], &end, 10);
    if (!argv[1][0] || *end || page > 2) return -EINVAL;
    atomic_set(&requested_page, page);
    shell_print(sh, "UI_PAGE_QUEUED");
    return 0;
}
SHELL_CMD_ARG_REGISTER(box3_page, NULL, "Select Home0/Server1/Weather2", page_command, 2, 0);

static int weather_command(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    char *end;
    unsigned long pressure = strtoul(argv[1], &end, 10);
    if (!argv[1][0] || *end || pressure > 1) return -EINVAL;
    unsigned long hours = strtoul(argv[2], &end, 10);
    if (!argv[2][0] || *end || (hours != 24 && hours != 48)) return -EINVAL;
    atomic_set(&requested_weather, pressure * 100 + hours);
    shell_print(sh, "UI_WEATHER_QUEUED");
    return 0;
}
SHELL_CMD_ARG_REGISTER(box3_weather, NULL, "Select temperature0/pressure1 and 24/48 hours", weather_command, 3, 0);

static void touch_event(struct input_event *event, void *data)
{
    ARG_UNUSED(data);
    static int32_t x, y;
    static bool pressed;
    if (event->type == INPUT_EV_ABS && event->code == INPUT_ABS_X) x = event->value;
    else if (event->type == INPUT_EV_ABS && event->code == INPUT_ABS_Y) y = event->value;
    else if (event->type == INPUT_EV_KEY && event->code == INPUT_BTN_TOUCH &&
             pressed != (event->value != 0)) {
        pressed = event->value != 0;
        printk("BOX3_TOUCH pressed=%d x=%d y=%d\n", pressed, x, y);
    }
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)), touch_event, NULL);

static uint8_t aht_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xff;

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static int start_aht30(void)
{
    const uint8_t trigger[] = {0xac, 0x33, 0x00};
    return device_is_ready(dock) ? i2c_write(dock, trigger, sizeof(trigger), AHT30_ADDRESS) : -ENODEV;
}

static int read_aht30(int32_t *temp_mc, uint32_t *rh_milli)
{
    uint8_t data[7];
    int ret = i2c_read(dock, data, sizeof(data), AHT30_ADDRESS);
    if (ret < 0) {
        return ret;
    }
    if (data[0] & 0x80) {
        return -EBUSY;
    }
    if (aht_crc(data, 6) != data[6]) {
        return -EBADMSG;
    }
    uint32_t humidity = ((uint32_t)data[1] << 12) |
                        ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t temperature = ((uint32_t)(data[3] & 0x0f) << 16) |
                           ((uint32_t)data[4] << 8) | data[5];

    *temp_mc = (int32_t)(((uint64_t)temperature * 200000) >> 20) - 50000;
    *rh_milli = (uint32_t)(((uint64_t)humidity * 100000) >> 20);
    return 0;
}

int main(void)
{
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    bool screen = device_is_ready(display);
    bool room_ok = false, sampling = false;
    int32_t temperature = 0;
    uint32_t humidity = 0;
    unsigned int samples = 0;
    int64_t next_sample = 0, next_ui = 0, next_health = 0;
    printk("\nBOX3_DASHBOARD Zephyr %s display=%d touch=%d\n",
           KERNEL_VERSION_STRING, screen, device_is_ready(touch));
    if (screen) {
        struct display_capabilities caps;
        display_get_capabilities(display, &caps);
        printk("BOX3_DISPLAY width=%u height=%u orientation=%d\n", caps.x_resolution, caps.y_resolution, caps.current_orientation);
        lv_display_add_event_cb(lv_display_get_default(), render_event, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(lv_display_get_default(), render_event, LV_EVENT_RENDER_READY, NULL);
        ui_init();
        lv_mem_monitor_t memory;
        lv_mem_monitor(&memory);
        printk("BOX3_UI heap_used_pct=%u max_bytes=%zu first_frame_start\n", memory.used_pct, memory.max_used);
        lv_timer_handler();
        lv_mem_monitor(&memory);
        printk("BOX3_UI first_frame_done heap_used_pct=%u max_bytes=%zu\n", memory.used_pct, memory.max_used);
        printk("BOX3_DISPLAY blanking_off=%d\n", display_blanking_off(display));
    }
    while (1) {
        int64_t now = k_uptime_get();
        if (now >= next_sample) {
            int ret;
            if (sampling) {
                ret = read_aht30(&temperature, &humidity);
                sampling = false;
            } else {
                ret = start_aht30();
                sampling = ret == 0;
            }
            if (sampling) {
                /* The sensor converts while the main thread keeps rendering. */
                next_sample = k_uptime_get() + 100;
            } else {
                room_ok = ret == 0;
                telemetry_sample(ret, temperature, humidity);
                printk("BOX3_PROBE sample=%u result=%d temperature_mC=%d humidity_milli_percent=%u%s\n",
                       ++samples, ret, temperature, humidity, room_ok ? " CRC=OK" : "");
                next_sample = k_uptime_get() + 2000;
            }
        }
        if (screen) {
            atomic_val_t page = atomic_set(&requested_page, -1);
            if (page >= 0) ui_show_page(page);
            atomic_val_t weather = atomic_set(&requested_weather, -1);
            if (weather >= 0) ui_weather_set(weather / 100, weather % 100);
            if (now >= next_ui) {
                struct dashboard_data dashboard;
                bool have = dashboard_copy(&dashboard);
                ui_update(room_ok, temperature, humidity, radio_is_online(), telemetry_sent(),
                          &dashboard, have, k_uptime_get());
                next_ui = k_uptime_get() + 1000;
            }
            lv_timer_handler();
            if (now >= next_health) {
                size_t unused = 0;
                lv_mem_monitor_t memory;
                lv_mem_monitor(&memory);
                int ret = k_thread_stack_space_get(k_current_get(), &unused);
                printk("BOX3_UI health heap_used_pct=%u peak_bytes=%zu stack_free=%zu stack_info=%d\n",
                       memory.used_pct, memory.max_used, unused, ret);
                printk("BOX3_UI render frames=%u avg_us=%u max_us=%u\n", render_count,
                       render_count ? render_total_us / render_count : 0, render_max_us);
                render_count = render_total_us = render_max_us = 0;
                ui_pointer_debug();
                next_health = k_uptime_get() + 10000;
            }
        }
        k_msleep(10);
    }
}
