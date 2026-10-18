#include <errno.h>
#include <string.h>
#include <limits.h>
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "dashboard.h"

static K_MUTEX_DEFINE(dashboard_lock);
static struct dashboard_data current;
static bool ready;
#define P(field) JSON_OBJ_DESCR_PRIM(struct forecast_point, field, JSON_TOK_NUMBER)
static const struct json_obj_descr point_fields[] = {
    P(m), P(t), P(p), P(lo), P(hi), P(pl), P(ph),
};
#undef P
#define F(field, type) JSON_OBJ_DESCR_PRIM(struct forecast_data, field, type)
static const struct json_obj_descr forecast_fields[] = {
    F(ok, JSON_TOK_TRUE), F(age_secs, JSON_TOK_NUMBER), F(source_age_secs, JSON_TOK_NUMBER),
    F(temp_centi, JSON_TOK_NUMBER), F(pressure_dhpa, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_OBJ_ARRAY(struct forecast_data, points, BOX3_FORECAST_POINTS, count,
                            point_fields, ARRAY_SIZE(point_fields)),
};
#undef F

static void initialize_forecast(struct forecast_data *f)
{
    memset(f, 0, sizeof(*f));
    f->age_secs = f->source_age_secs = f->temp_centi = f->pressure_dhpa = INT32_MIN;
    for (size_t i = 0; i < BOX3_FORECAST_POINTS; ++i)
        f->points[i] = (struct forecast_point){INT32_MIN, INT32_MIN, INT32_MIN,
            INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
}
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
    JSON_OBJ_DESCR_OBJECT(struct dashboard_data, forecast, forecast_fields),
};
#undef D

int dashboard_accept(char *body, size_t length)
{
    struct dashboard_data next = {0};
    initialize_forecast(&next.forecast);
    int64_t decoded = json_obj_parse(body, length, fields, ARRAY_SIZE(fields), &next);
    const int64_t core_mask = BIT64(ARRAY_SIZE(fields) - 1) - 1;
    bool has_forecast = decoded >= 0 && (decoded & BIT64(ARRAY_SIZE(fields) - 1));
    if (decoded < 0 || (decoded & core_mask) != core_mask
        || next.cpu_milli_percent < 0 || next.cpu_milli_percent > 100000
        || next.mem_used_kib < 0 || next.mem_total_kib < next.mem_used_kib
        || next.swap_used_kib < 0 || next.swap_total_kib < next.swap_used_kib
        || next.uptime_secs < 0 || next.load_milli < 0 || next.metrics_age_secs < 0
        || next.weather_age_secs < 0 || next.weather_temp_mc < -80000
        || next.weather_temp_mc > 65000 || next.weather_feels_mc < -80000
        || next.weather_feels_mc > 65000 || next.weather_humidity_percent < 0
        || next.weather_humidity_percent > 100 || next.weather_kind < 0
        || next.weather_kind > 6 || (next.metrics_ok && next.mem_total_kib == 0)
        || (has_forecast && !forecast_valid(&next.forecast))) {
        return -EBADMSG;
    }
    for (size_t i = 0; next.weather_text[i]; ++i) {
        if (next.weather_text[i] < 32 || next.weather_text[i] > 126) {
            return -EBADMSG;
        }
    }
    k_mutex_lock(&dashboard_lock, K_FOREVER);
    if (!has_forecast) next.forecast = current.forecast;
    else next.forecast.received_uptime_ms = k_uptime_get();
    next.received_uptime_ms = k_uptime_get();
    next.generation = current.generation + 1;
    current = next;
    ready = true;
    k_mutex_unlock(&dashboard_lock);
    return 0;
}

int dashboard_accept_forecast(char *body, size_t length, bool local)
{
    struct forecast_data next;
    initialize_forecast(&next);
    int64_t decoded = json_obj_parse(body, length, forecast_fields, ARRAY_SIZE(forecast_fields), &next);
    if (decoded != (BIT64(ARRAY_SIZE(forecast_fields)) - 1) || !forecast_valid(&next)) return -EBADMSG;
    next.received_uptime_ms = k_uptime_get();
    next.local = local;
    k_mutex_lock(&dashboard_lock, K_FOREVER);
    current.forecast = next;
    current.generation++;
    k_mutex_unlock(&dashboard_lock);
    return 0;
}

/* Trusted USB diagnostics use the same bounded JSON parser as HTTPS. */
static char forecast_staging[3073];
static size_t staged_length;
static bool staging_open;
static int forecast_begin(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    staged_length = 0; staging_open = true;
    shell_print(sh, "FORECAST_BEGIN_OK");
    return 0;
}
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int forecast_chunk(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    size_t length = strlen(argv[1]);
    if (!staging_open || !length || length > 160 || length % 2
        || staged_length + length / 2 >= sizeof(forecast_staging)) return -EINVAL;
    for (size_t i = 0; i < length; i += 2) {
        int a = hex_digit(argv[1][i]), b = hex_digit(argv[1][i + 1]);
        if (a < 0 || b < 0) { staging_open = false; return -EINVAL; }
        forecast_staging[staged_length++] = (char)((a << 4) | b);
    }
    shell_print(sh, "FORECAST_CHUNK_OK %u", (unsigned)staged_length);
    return 0;
}
static int forecast_commit(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    if (!staging_open) return -EINVAL;
    staging_open = false;
    forecast_staging[staged_length] = 0;
    int result = dashboard_accept_forecast(forecast_staging, staged_length, true);
    shell_print(sh, "BOX3_FORECAST result=%d bytes=%u transport=USB", result, (unsigned)staged_length);
    return result;
}
SHELL_CMD_ARG_REGISTER(box3_forecast_begin, NULL, "Begin trusted USB forecast transfer", forecast_begin, 1, 0);
SHELL_CMD_ARG_REGISTER(box3_forecast_chunk, NULL, "Append up to 80 bytes as lowercase hex", forecast_chunk, 2, 0);
SHELL_CMD_ARG_REGISTER(box3_forecast_commit, NULL, "Validate and display the USB forecast", forecast_commit, 1, 0);

static int forecast_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    struct dashboard_data data;
    dashboard_copy(&data);
    const struct forecast_data *f = &data.forecast;
    uint32_t hash = 2166136261u;
    const unsigned char *bytes = (const unsigned char *)f->points;
    for (size_t i = 0; i < f->count * sizeof(f->points[0]); ++i) hash = (hash ^ bytes[i]) * 16777619u;
    int64_t elapsed = (k_uptime_get() - f->received_uptime_ms) / 1000;
    shell_print(sh, "FORECAST_STATE ok=%u points=%u local=%u hash=%08x age=%lld source_age=%lld",
                f->ok, (unsigned)f->count, f->local, (unsigned)hash,
                (long long)(f->age_secs + elapsed), (long long)(f->source_age_secs + elapsed));
    return 0;
}
SHELL_CMD_ARG_REGISTER(box3_forecast_status, NULL, "Read forecast status without exposing credentials", forecast_status, 1, 0);

bool dashboard_copy(struct dashboard_data *out)
{
    k_mutex_lock(&dashboard_lock, K_FOREVER);
    bool ok = ready;
    *out = current;
    k_mutex_unlock(&dashboard_lock);
    return ok;
}
