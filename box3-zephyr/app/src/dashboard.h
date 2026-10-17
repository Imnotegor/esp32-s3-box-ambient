#ifndef BOX3_DASHBOARD_H
#define BOX3_DASHBOARD_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct dashboard_data {
    bool metrics_ok;
    int32_t cpu_milli_percent, mem_used_kib, mem_total_kib;
    int32_t swap_used_kib, swap_total_kib, uptime_secs, load_milli, metrics_age_secs;
    bool weather_ok;
    int32_t weather_temp_mc, weather_feels_mc, weather_humidity_percent, weather_kind;
    char weather_text[41];
    int32_t weather_age_secs;
    bool weather_stale;
    int64_t received_uptime_ms;
    uint32_t generation;
};
int dashboard_accept(char *body, size_t length);
bool dashboard_copy(struct dashboard_data *out);
#endif
