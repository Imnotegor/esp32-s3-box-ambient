/* Synthetic design fixtures only; the device UI never invents metric history. */
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui.h"
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *data)
{
    (void)area; (void)data;
    lv_display_flush_ready(d);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 1;
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
    for (int i = 0; i < 32; ++i) {
        d.generation++;
        d.cpu_milli_percent = (i < 15 ? 7000+i*1100 : 26000-(i-15)*500);
        ui_update(true, 26400, 45300, true, true, &d, true, 1000);
    }
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    printf("UI_HEAP used=%zu free=%zu\n", memory.total_size - memory.free_size, memory.free_size);
    for (unsigned i = 0; i < 3; ++i) {
        ui_show_page(i);
        lv_obj_update_layout(lv_screen_active());
        lv_tick_inc(40);
        lv_timer_handler();
        lv_draw_buf_t *snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        if (!snapshot) return 2;
        char path[512];
        snprintf(path, sizeof(path), "%s/page%u.raw", argv[1], i);
        FILE *file = fopen(path, "wb");
        if (!file) return 3;
        fwrite(snapshot->data, 1, snapshot->header.stride * snapshot->header.h, file);
        fclose(file);
        printf("FRAME %u %u %u %u\n", i, snapshot->header.w, snapshot->header.h, snapshot->header.stride);
        lv_draw_buf_destroy(snapshot);
    }
    return 0;
}
