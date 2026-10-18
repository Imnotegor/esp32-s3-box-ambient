/* Synthetic design fixtures only; the device UI never invents metric history. */
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
static uint16_t frame[320 * 240], expected[3][320 * 240];
static unsigned flushes;

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(4);
    }
}

static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *data)
{
    check(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 320 && area->y2 < 240,
          "flush outside display");
    size_t width = area->x2 - area->x1 + 1;
    size_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    for (int y = area->y1; y <= area->y2; ++y)
        memcpy(&frame[y * 320 + area->x1], data + (y - area->y1) * stride, width * 2);
    flushes++;
    lv_display_flush_ready(d);
}

static void advance(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 16) {
        lv_tick_inc(16);
        lv_timer_handler();
    }
}

static void save_frame(const char *directory, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.raw", directory, name);
    FILE *file = fopen(path, "wb");
    check(file != NULL, "open preview output");
    check(fwrite(frame, sizeof(frame), 1, file) == 1, "write preview output");
    check(fclose(file) == 0, "close preview output");
}
int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 1;
    lv_init();
    lv_display_t *display = lv_display_create(320, 240);
    static uint8_t buffer[320 * 32 * 2];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    ui_init();
    struct dashboard_data d = {
        .metrics_ok = true, .cpu_milli_percent = 17400,
        .mem_used_kib = 6314000, .mem_total_kib = 16117180,
        .swap_used_kib = 760000, .swap_total_kib = 16510972,
        .uptime_secs = 26740, .load_milli = 420,
        .weather_ok = true, .weather_temp_mc = 15000, .weather_feels_mc = 12000,
        .weather_humidity_percent = 78, .weather_kind = 1, .weather_text = "Partly cloudy",
        .weather_age_secs = 180, .received_uptime_ms = 1000,
    };
    d.forecast.ok = true; d.forecast.count = BOX3_FORECAST_POINTS;
    d.forecast.temp_centi = 1500; d.forecast.pressure_dhpa = 10150;
    d.forecast.age_secs = 60; d.forecast.source_age_secs = 180;
    d.forecast.received_uptime_ms = 1000;
    unsigned point = 0;
    for (int hour = 0; hour < 48; ++hour) {
        if (!(hour < 23 && hour % 2 == 0 || hour >= 23 && hour % 2 == 1)) continue;
        int t = 1500 - (hour % 24 < 12 ? hour % 24 : 24 - hour % 24) * 40;
        int p = 10150 - hour * 2;
        d.forecast.points[point++] = (struct forecast_point){(hour + 1) * 60, t, p, t - 200, t + 200, p - 40, p + 40};
    }
    if (argc == 3) {
        FILE *input = fopen(argv[2], "r");
        check(input != NULL, "open actual forecast fixture");
        check(fscanf(input, "%d %d %d %d", &d.forecast.age_secs, &d.forecast.source_age_secs,
                     &d.forecast.temp_centi, &d.forecast.pressure_dhpa) == 4, "forecast metadata");
        for (size_t i = 0; i < BOX3_FORECAST_POINTS; ++i) {
            struct forecast_point *p = &d.forecast.points[i];
            check(fscanf(input, "%d %d %d %d %d %d %d", &p->m, &p->t, &p->p, &p->lo, &p->hi, &p->pl, &p->ph) == 7, "forecast point");
        }
        check(fclose(input) == 0, "close forecast fixture");
        d.forecast.local = true;
    }
    check(forecast_valid(&d.forecast), "forecast protocol bounds");
    struct forecast_data invalid = d.forecast;
    invalid.points[1].m = 0; check(!forecast_valid(&invalid), "reject backwards forecast time");
    invalid = d.forecast; invalid.points[0].lo = 16000; check(!forecast_valid(&invalid), "reject invalid uncertainty range");
    for (int i = 0; i < 32; ++i) {
        d.generation++;
        d.cpu_milli_percent = (i < 15 ? 7000+i*1100 : 26000-(i-15)*500);
        ui_update(true, 26400, 45300, true, true, &d, true, 1000);
    }

    for (unsigned i = 0; i < 3; ++i) {
        ui_show_page(i);
        lv_obj_update_layout(lv_screen_active());
        advance(400);
        lv_draw_buf_t *snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        check(snapshot && snapshot->header.stride == 640, "RGB565 snapshot");
        check(!memcmp(frame, snapshot->data, sizeof(frame)), "partial flush matches complete page");
        memcpy(expected[i], frame, sizeof(frame));
        char name[16];
        snprintf(name, sizeof(name), "page%u", i);
        save_frame(argv[1], name);
        printf("FRAME %u %u %u %u\n", i, snapshot->header.w, snapshot->header.h, snapshot->header.stride);
        lv_draw_buf_destroy(snapshot);
    }

    ui_show_page(2); advance(400);
    for (unsigned metric = 0; metric < 2; ++metric) {
        for (unsigned hours = 24; hours <= 48; hours += 24) {
            ui_weather_set(metric, hours); advance(64);
            char name[32]; snprintf(name, sizeof(name), "weather-%s-%u", metric ? "pressure" : "temperature", hours);
            save_frame(argv[1], name);
        }
    }
    ui_weather_set(0, 48); advance(64);
    check(!memcmp(frame, expected[2], sizeof(frame)), "weather controls restore the same data view");

    ui_show_page(0);
    advance(400);
    save_frame(argv[1], "motion00");
    ui_show_page(1);
    for (unsigned i = 1; i <= 16; ++i) {
        advance(16);
        char name[16];
        snprintf(name, sizeof(name), "motion%02u", i);
        save_frame(argv[1], name);
    }
    check(!memcmp(frame, expected[1], sizeof(frame)), "transition reaches exact destination");

    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    for (unsigned i = 0; i < 30; ++i) {
        ui_show_page(2);
        advance(48);
        ui_show_page(0);
        ui_show_page(1);
        ui_show_page(0);
        advance(500);
        check(!memcmp(frame, expected[0], sizeof(frame)), "rapid navigation keeps latest request");
        ui_show_page(1);
        advance(400);
    }
    lv_mem_monitor(&after);
    check(after.free_size == before.free_size, "repeated transitions do not leak memory");
    unsigned previous_flushes = flushes;
    ui_show_page(1);
    ui_show_page(3);
    advance(200);
    check(flushes == previous_flushes, "current or invalid page does not repaint");

    ui_show_page(2);
    advance(48);
    ui_update(false, 0, 0, false, false, &d, false, 1000);
    advance(400);
    lv_draw_buf_t *updated = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    check(updated && !memcmp(frame, updated->data, sizeof(frame)),
          "data changing during motion is visible when the page settles");
    lv_draw_buf_destroy(updated);
    printf("MOTION_CHECKS PASS heap_used=%zu flushes=%u\n", after.total_size - after.free_size, flushes);
    return 0;
}
