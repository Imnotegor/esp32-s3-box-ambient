#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

#define BG 0x090E1A
#define CARD 0x141D2E
#define BORDER 0x26354C
#define TEXT 0xEAF2FC
#define MUTED 0x8195AE
#define CYAN 0x42E6D0
#define PURPLE 0xB99AFF
#define GOLD 0xFFD166
#define RED 0xFB7185

struct weather_icon {
    lv_obj_t *root, *sun, *cloud, *rain, *snow, *storm, *fog, *unknown;
};
static lv_obj_t *pages[3], *tabs[3], *tab_labels[3], *header, *link_text, *link_dot;
static lv_obj_t *room_temp, *room_humidity, *room_status;
static lv_obj_t *mini_cpu, *mini_ram, *mini_bar, *mini_weather, *mini_feels;
static lv_obj_t *cpu_value, *cpu_history_title, *chart, *ram_value, *ram_pct, *ram_bar;
static lv_obj_t *swap_value, *swap_pct, *swap_bar, *server_footer;
static lv_obj_t *weather_temp, *weather_desc, *weather_feels, *weather_humidity, *weather_footer;
static lv_chart_series_t *cpu_series;
static struct weather_icon small_icon, large_icon;
static unsigned selected;
static uint32_t plotted_generation;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = box(parent, x, y, w, h, CARD, 12);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(BORDER), 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font,
                       uint32_t color, const char *text)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_pos(o, x, y);
    lv_label_set_text(o, text);
    return o;
}

static void text(lv_obj_t *o, const char *fmt, ...)
{
    char buffer[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (strcmp(lv_label_get_text(o), buffer)) {
        lv_label_set_text(o, buffer);
    }
}

static void visible(lv_obj_t *o, bool value)
{
    lv_obj_set_hidden(o, !value);
}

static lv_obj_t *group(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = box(parent, x, y, w, h, BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    return o;
}

static struct weather_icon icon(lv_obj_t *parent, int x, int y, int scale)
{
    struct weather_icon v = {0};
    v.root = group(parent, x, y, 46 * scale, 52 * scale);
    v.sun = group(v.root, 0, 0, 46 * scale, 52 * scale);
    box(v.sun, 15*scale, 12*scale, 18*scale, 18*scale, GOLD, 18*scale);
    box(v.sun, 22*scale, 4*scale, 3*scale, 5*scale, GOLD, scale);
    box(v.sun, 22*scale, 33*scale, 3*scale, 5*scale, GOLD, scale);
    box(v.sun, 7*scale, 20*scale, 5*scale, 3*scale, GOLD, scale);
    box(v.sun, 36*scale, 20*scale, 5*scale, 3*scale, GOLD, scale);
    v.cloud = group(v.root, 0, 0, 46*scale, 52*scale);
    box(v.cloud, 4*scale, 21*scale, 19*scale, 19*scale, 0xB9CCE2, 19*scale);
    box(v.cloud, 15*scale, 12*scale, 24*scale, 24*scale, 0xD9E5F3, 24*scale);
    box(v.cloud, 30*scale, 24*scale, 14*scale, 14*scale, 0xD9E5F3, 14*scale);
    box(v.cloud, 10*scale, 29*scale, 30*scale, 11*scale, 0xD9E5F3, 5*scale);
    v.rain = group(v.root, 0, 0, 46*scale, 52*scale);
    v.snow = group(v.root, 0, 0, 46*scale, 52*scale);
    for (int i = 0; i < 3; ++i) {
        box(v.rain, (10+12*i)*scale, 43*scale, 3*scale, 7*scale, CYAN, scale);
        label(v.snow, (8+12*i)*scale, 39*scale, &lv_font_montserrat_12, TEXT, "*");
    }
    v.storm = group(v.root, 0, 0, 46*scale, 52*scale);
    label(v.storm, 17*scale, 29*scale, &lv_font_montserrat_24, GOLD, LV_SYMBOL_CHARGE);
    v.fog = group(v.root, 0, 0, 46*scale, 52*scale);
    for (int i = 0; i < 3; ++i)
        box(v.fog, (5+i%2*4)*scale, (16+9*i)*scale, 33*scale, 3*scale, MUTED, scale);
    v.unknown = group(v.root, 0, 0, 46*scale, 52*scale);
    label(v.unknown, 14*scale, 10*scale, &lv_font_montserrat_24, MUTED, "?");
    return v;
}

static void icon_set(struct weather_icon *v, bool ok, int kind, bool stale)
{
    visible(v->root, ok);
    visible(v->sun, kind == 0);
    visible(v->cloud, kind >= 1 && kind <= 4);
    visible(v->rain, kind == 2);
    visible(v->snow, kind == 3);
    visible(v->storm, kind == 4);
    visible(v->fog, kind == 5);
    visible(v->unknown, kind == 6);
    lv_obj_set_style_opa(v->root, stale ? LV_OPA_50 : LV_OPA_COVER, 0);
}

static lv_obj_t *bar(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *o = lv_bar_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, width, height);
    lv_obj_set_style_radius(o, height, LV_PART_MAIN);
    lv_obj_set_style_radius(o, height, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x253149), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(o, 0, 100);
    return o;
}

void ui_show_page(unsigned page)
{
    if (page > 2) return;
    selected = page;
    const char *titles[] = {"S3 / HOME", "SERVER / SYSTEM", "OUTDOOR / WEATHER"};
    text(header, "%s", titles[page]);
    for (unsigned i = 0; i < 3; ++i) {
        visible(pages[i], i == page);
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(i == page ? 0x263852 : 0x101829), 0);
        lv_obj_set_style_text_color(tab_labels[i], lv_color_hex(i == page ? CYAN : MUTED), 0);
    }
    printf("BOX3_UI page=%u\n", page);
}

void ui_pointer_debug(void)
{
    lv_indev_t *input = NULL;
    while ((input = lv_indev_get_next(input))) {
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) {
            lv_point_t point;
            lv_indev_get_point(input, &point);
            printf("BOX3_POINTER x=%ld y=%ld state=%d\n", (long)point.x, (long)point.y, lv_indev_get_state(input));
        }
    }
}

static void navigation(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        unsigned page = (unsigned)(uintptr_t)lv_event_get_user_data(e);
        printf("BOX3_NAV tap=%u\n", page);
        ui_show_page(page);
    } else if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_indev_t *input = lv_indev_active();
        if (!input) return;
        lv_dir_t direction = lv_indev_get_gesture_dir(input);
        if (direction == LV_DIR_LEFT) {
            printf("BOX3_NAV swipe=left\n");
            ui_show_page((selected + 1) % 3);
        }
        if (direction == LV_DIR_RIGHT) {
            printf("BOX3_NAV swipe=right\n");
            ui_show_page((selected + 2) % 3);
        }
    }
}

void ui_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);
    /* One root handler also receives swipes over non-clickable card backgrounds. */
    lv_obj_add_event_cb(screen, navigation, LV_EVENT_GESTURE, NULL);
    box(screen, 0, 0, 106, 2, CYAN, 0);
    box(screen, 106, 0, 108, 2, PURPLE, 0);
    box(screen, 214, 0, 106, 2, GOLD, 0);
    header = label(screen, 12, 9, &lv_font_montserrat_12, TEXT, "S3 / HOME");
    link_dot = box(screen, 260, 13, 6, 6, MUTED, 6);
    link_text = label(screen, 274, 8, &lv_font_montserrat_12, MUTED, "WAIT");
    for (int i = 0; i < 3; ++i) {
        pages[i] = group(screen, 0, 28, 320, 174);
    }
    lv_obj_t *o = card(pages[0], 10, 4, 146, 78);
    label(o, 10, 8, &lv_font_montserrat_12, MUTED, "ROOM / " LV_SYMBOL_HOME);
    room_temp = label(o, 8, 25, &lv_font_montserrat_32, CYAN, "--");
    room_status = label(o, 10, 61, &lv_font_montserrat_12, MUTED, "AHT30 / waiting");
    o = card(pages[0], 164, 4, 146, 78);
    label(o, 10, 8, &lv_font_montserrat_12, MUTED, "HUMIDITY");
    room_humidity = label(o, 10, 27, &lv_font_montserrat_32, PURPLE, "--");
    label(o, 10, 61, &lv_font_montserrat_12, MUTED, "relative / % RH");
    o = card(pages[0], 10, 88, 146, 82);
    label(o, 10, 8, &lv_font_montserrat_12, PURPLE, "SERVER");
    mini_cpu = label(o, 10, 27, &lv_font_montserrat_14, TEXT, "CPU --");
    mini_bar = bar(o, 10, 49, 124, 5, PURPLE);
    mini_ram = label(o, 10, 62, &lv_font_montserrat_12, MUTED, "RAM --");
    o = card(pages[0], 164, 88, 146, 82);
    label(o, 10, 8, &lv_font_montserrat_12, GOLD, "OUTDOOR");
    mini_weather = label(o, 10, 29, &lv_font_montserrat_24, TEXT, "--");
    mini_feels = label(o, 10, 62, &lv_font_montserrat_12, MUTED, "FEELS --");
    small_icon = icon(o, 91, 15, 1);

    cpu_history_title = label(pages[1], 12, 7, &lv_font_montserrat_12, PURPLE, "CPU / LIVE HISTORY");
    cpu_value = label(pages[1], 204, 1, &lv_font_montserrat_24, TEXT, "--");
    lv_obj_set_width(cpu_value, 104);
    lv_obj_set_style_text_align(cpu_value, LV_TEXT_ALIGN_RIGHT, 0);
    chart = lv_chart_create(pages[1]);
    lv_obj_remove_style_all(chart);
    lv_obj_set_pos(chart, 10, 33);
    lv_obj_set_size(chart, 300, 58);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100000);
    lv_chart_set_point_count(chart, 32);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    cpu_series = lv_chart_add_series(chart, lv_color_hex(PURPLE), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(chart, cpu_series, LV_CHART_POINT_NONE);
    ram_value = label(pages[1], 12, 101, &lv_font_montserrat_12, TEXT, "RAM --");
    ram_pct = label(pages[1], 248, 101, &lv_font_montserrat_12, CYAN, "--");
    ram_bar = bar(pages[1], 12, 119, 296, 7, CYAN);
    swap_value = label(pages[1], 12, 134, &lv_font_montserrat_12, TEXT, "SWAP --");
    swap_pct = label(pages[1], 248, 134, &lv_font_montserrat_12, GOLD, "--");
    swap_bar = bar(pages[1], 12, 151, 296, 6, GOLD);
    server_footer = label(pages[1], 12, 162, &lv_font_montserrat_12, MUTED, "Waiting for server");

    label(pages[2], 14, 6, &lv_font_montserrat_12, GOLD, "CURRENT CONDITIONS");
    weather_temp = label(pages[2], 12, 28, &lv_font_montserrat_32, TEXT, "--");
    weather_desc = label(pages[2], 14, 72, &lv_font_montserrat_14, MUTED, "Waiting for weather");
    lv_obj_set_size(weather_desc, 190, 32);
    lv_label_set_long_mode(weather_desc, LV_LABEL_LONG_MODE_DOTS);
    large_icon = icon(pages[2], 212, 2, 2);
    o = card(pages[2], 10, 108, 146, 49);
    label(o, 10, 5, &lv_font_montserrat_12, MUTED, "FEELS LIKE");
    weather_feels = label(o, 10, 21, &lv_font_montserrat_24, GOLD, "--");
    o = card(pages[2], 164, 108, 146, 49);
    label(o, 10, 5, &lv_font_montserrat_12, MUTED, "HUMIDITY");
    weather_humidity = label(o, 10, 21, &lv_font_montserrat_24, CYAN, "--");
    weather_footer = label(pages[2], 14, 162, &lv_font_montserrat_12, MUTED, "WEATHER / SOURCE");

    lv_obj_t *nav = box(screen, 0, 204, 320, 36, 0x101829, 0);
    const char *names[] = {"HOME", "SERVER", "WEATHER"};
    for (unsigned i = 0; i < 3; ++i) {
        tabs[i] = lv_button_create(nav);
        lv_obj_remove_style_all(tabs[i]);
        lv_obj_set_pos(tabs[i], 6 + i * 105, 2);
        lv_obj_set_size(tabs[i], 98, 32);
        lv_obj_set_style_radius(tabs[i], 8, 0);
        lv_obj_set_style_bg_opa(tabs[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(0x314865), LV_STATE_PRESSED);
        tab_labels[i] = label(tabs[i], 0, 0, &lv_font_montserrat_12, MUTED, names[i]);
        lv_obj_center(tab_labels[i]);
        lv_obj_add_event_cb(tabs[i], navigation, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    icon_set(&small_icon, false, 6, false);
    icon_set(&large_icon, false, 6, false);
    ui_show_page(0);
}

static unsigned ratio(int32_t used, int32_t total)
{
    return total > 0 ? (unsigned)((uint64_t)used * 100 / total) : 0;
}

static unsigned gib10(int32_t kib)
{
    return (unsigned)((uint64_t)kib * 10 / (1024 * 1024));
}

void ui_update(bool room_ok, int32_t temperature_mc, uint32_t humidity,
               bool wifi, bool uploaded, const struct dashboard_data *d,
               bool have, int64_t now)
{
    bool link = have && now - d->received_uptime_ms <= 15000;
    bool metrics = link && d->metrics_ok;
    uint32_t color = !wifi ? RED : link ? CYAN : GOLD;
    lv_obj_set_style_bg_color(link_dot, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(link_text, lv_color_hex(color), 0);
    text(link_text, "%s", !wifi ? "OFF" : link ? "LIVE" : "WAIT");
    if (room_ok) {
        int32_t n = temperature_mc < 0 ? -temperature_mc : temperature_mc;
        n = (n + 50) / 100;
        unsigned rh = (humidity + 50) / 100;
        text(room_temp, "%s%d.%d°", temperature_mc < 0 ? "-" : "", n / 10, n % 10);
        text(room_humidity, "%u.%u%%", rh / 10, rh % 10);
        text(room_status, "%s", uploaded ? "AHT30 / uplink OK" : "AHT30 / local");
    } else {
        text(room_temp, "--"); text(room_humidity, "--"); text(room_status, "SENSOR unavailable");
    }
    lv_obj_t *metric_objects[] = {mini_bar, ram_bar, swap_bar, chart};
    for (unsigned i = 0; i < 4; ++i)
        lv_obj_set_style_opa(metric_objects[i], metrics ? LV_OPA_COVER : LV_OPA_30, 0);
    if (metrics) {
        int cpu = d->cpu_milli_percent;
        unsigned memory = ratio(d->mem_used_kib, d->mem_total_kib);
        unsigned swap = ratio(d->swap_used_kib, d->swap_total_kib);
        unsigned used = gib10(d->mem_used_kib), total = gib10(d->mem_total_kib);
        unsigned cpu10 = (cpu + 50) / 100;
        text(mini_cpu, "CPU %u.%u%%", cpu10 / 10, cpu10 % 10);
        text(cpu_value, "%u.%u%%", cpu10 / 10, cpu10 % 10);
        text(mini_ram, "RAM %u.%u / %u.%uG", used/10, used%10, total/10, total%10);
        text(ram_value, "RAM  %u.%u / %u.%u GiB", used/10, used%10, total/10, total%10);
        text(ram_pct, "%u%%", memory);
        used = gib10(d->swap_used_kib); total = gib10(d->swap_total_kib);
        text(swap_value, d->swap_total_kib ? "SWAP  %u.%u / %u.%u GiB" : "SWAP  disabled",
             used/10, used%10, total/10, total%10);
        text(swap_pct, d->swap_total_kib ? "%u%%" : "--", swap);
        lv_bar_set_value(mini_bar, cpu/1000, LV_ANIM_OFF);
        lv_bar_set_value(ram_bar, memory, LV_ANIM_OFF);
        lv_bar_set_value(swap_bar, swap, LV_ANIM_OFF);
        if (d->generation != plotted_generation) {
            lv_chart_set_next_value(chart, cpu_series, cpu);
            int32_t *values = lv_chart_get_series_y_array(chart, cpu_series);
            int32_t peak = 0;
            for (unsigned i = 0; i < 32; ++i)
                if (values[i] != LV_CHART_POINT_NONE && values[i] > peak) peak = values[i];
            int32_t range = ((peak * 5 / 4 + 4999) / 5000) * 5000;
            if (range < 5000) range = 5000;
            if (range > 100000) range = 100000;
            lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, range);
            text(cpu_history_title, "CPU HISTORY / 0-%d%%", range/1000);
            plotted_generation = d->generation;
        }
        unsigned hours = d->uptime_secs / 3600;
        text(server_footer, "UP %uh %um   |   LOAD %d.%02d", hours,
             d->uptime_secs % 3600 / 60, d->load_milli/1000, d->load_milli%1000/10);
    } else {
        text(mini_cpu, "CPU --"); text(mini_ram, "RAM --"); text(cpu_value, "--");
        text(ram_value, "RAM  --"); text(ram_pct, "--");
        text(swap_value, "SWAP  --"); text(swap_pct, "--");
        text(server_footer, wifi ? "Server data pending / stale" : "Wi-Fi offline / chart retained");
    }
    bool weather = have && d->weather_ok;
    int64_t age = weather ? d->weather_age_secs + (now - d->received_uptime_ms) / 1000 : 0;
    bool old = weather && (d->weather_stale || age > 1800 || !link);
    icon_set(&small_icon, weather, d->weather_kind, old);
    icon_set(&large_icon, weather, d->weather_kind, old);
    if (weather) {
        text(mini_weather, "%+d°", d->weather_temp_mc / 1000);
        text(mini_feels, "FEELS %+d°", d->weather_feels_mc / 1000);
        text(weather_temp, "%+d°C", d->weather_temp_mc / 1000);
        text(weather_feels, "%+d°", d->weather_feels_mc / 1000);
        text(weather_humidity, "%d%%", d->weather_humidity_percent);
        text(weather_desc, "%s", d->weather_text);
        text(weather_footer, "%s %lldm  /  wttr.in", old ? "CACHED" : "UPDATED", (long long)age/60);
    } else {
        text(mini_weather, "--"); text(mini_feels, "FEELS --"); text(weather_temp, "--");
        text(weather_feels, "--"); text(weather_humidity, "--");
        text(weather_desc, "Weather unavailable");
        text(weather_footer, "WEATHER / waiting for source");
    }
}
