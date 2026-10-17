#include <errno.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include "dashboard.h"

static K_MUTEX_DEFINE(dashboard_lock);
static struct dashboard_data current;
static bool ready;
#define D(field, type) JSON_OBJ_DESCR_PRIM(struct dashboard_data, field, type)
static const struct json_obj_descr fields[] = {
    D(metrics_ok, JSON_TOK_TRUE),
    D(cpu_milli_percent, JSON_TOK_NUMBER),
    D(mem_used_kib, JSON_TOK_NUMBER), D(mem_total_kib, JSON_TOK_NUMBER),
    D(swap_used_kib, JSON_TOK_NUMBER), D(swap_total_kib, JSON_TOK_NUMBER),
    D(uptime_secs, JSON_TOK_NUMBER), D(load_milli, JSON_TOK_NUMBER),
    D(metrics_age_secs, JSON_TOK_NUMBER),
    D(weather_ok, JSON_TOK_TRUE),
    D(weather_temp_mc, JSON_TOK_NUMBER), D(weather_feels_mc, JSON_TOK_NUMBER),
    D(weather_humidity_percent, JSON_TOK_NUMBER), D(weather_kind, JSON_TOK_NUMBER),
    D(weather_text, JSON_TOK_STRING_BUF), D(weather_age_secs, JSON_TOK_NUMBER),
    D(weather_stale, JSON_TOK_TRUE),
};
#undef D

int dashboard_accept(char *body, size_t length)
{
    struct dashboard_data next = {0};
    int64_t decoded = json_obj_parse(body, length, fields, ARRAY_SIZE(fields), &next);
    if (decoded < 0 || decoded != (BIT64(ARRAY_SIZE(fields)) - 1)
        || next.cpu_milli_percent < 0 || next.cpu_milli_percent > 100000
        || next.mem_used_kib < 0 || next.mem_total_kib < next.mem_used_kib
        || next.swap_used_kib < 0 || next.swap_total_kib < next.swap_used_kib
        || next.uptime_secs < 0 || next.load_milli < 0 || next.metrics_age_secs < 0
        || next.weather_age_secs < 0 || next.weather_temp_mc < -80000
        || next.weather_temp_mc > 65000 || next.weather_feels_mc < -80000
        || next.weather_feels_mc > 65000 || next.weather_humidity_percent < 0
        || next.weather_humidity_percent > 100 || next.weather_kind < 0
        || next.weather_kind > 6 || (next.metrics_ok && next.mem_total_kib == 0)) {
        return -EBADMSG;
    }
    for (size_t i = 0; next.weather_text[i]; ++i) {
        if (next.weather_text[i] < 32 || next.weather_text[i] > 126) {
            return -EBADMSG;
        }
    }
    k_mutex_lock(&dashboard_lock, K_FOREVER);
    next.received_uptime_ms = k_uptime_get();
    next.generation = current.generation + 1;
    current = next;
    ready = true;
    k_mutex_unlock(&dashboard_lock);
    return 0;
}

bool dashboard_copy(struct dashboard_data *out)
{
    k_mutex_lock(&dashboard_lock, K_FOREVER);
    bool ok = ready;
    *out = current;
    k_mutex_unlock(&dashboard_lock);
    return ok;
}
