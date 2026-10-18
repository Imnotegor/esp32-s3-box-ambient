#ifndef BOX3_DASHBOARD_H
#define BOX3_DASHBOARD_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define BOX3_FORECAST_POINTS 25
struct forecast_point {
    int32_t m, t, p, lo, hi, pl, ph;
};
struct forecast_data {
    bool ok;
    int32_t age_secs, source_age_secs, temp_centi, pressure_dhpa;
    struct forecast_point points[BOX3_FORECAST_POINTS];
    size_t count;
    bool local;
    int64_t received_uptime_ms;
};
bool forecast_valid(const struct forecast_data *data);

struct dashboard_data {
    bool metrics_ok;
    int32_t cpu_milli_percent, mem_used_kib, mem_total_kib;
    int32_t swap_used_kib, swap_total_kib, uptime_secs, load_milli, metrics_age_secs;
    bool weather_ok;
    int32_t weather_temp_mc, weather_feels_mc, weather_humidity_percent, weather_kind;
    char weather_text[41];
    int32_t weather_age_secs;
    bool weather_stale;
    struct forecast_data forecast;
    int64_t received_uptime_ms;
    uint32_t generation;
};
int dashboard_accept(char *body, size_t length);
bool dashboard_copy(struct dashboard_data *out);
int dashboard_accept_forecast(char *body, size_t length, bool local);
#endif
