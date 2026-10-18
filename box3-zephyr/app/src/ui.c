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
#define PAGE_COUNT 3
#define PAGE_WIDTH 320
#define PAGE_HEIGHT 174
#define PAGE_TRANSITION_MS 160
#define PAGE_ENTER_DISTANCE 16

struct weather_icon {
    lv_obj_t *root, *sun, *cloud, *rain, *snow, *storm, *fog, *unknown;
};
static lv_obj_t *pages[3], *tabs[3], *tab_labels[3], *header, *link_text, *link_dot, *tab_marker;
static lv_obj_t *room_temp, *room_humidity, *room_status;
static lv_obj_t *mini_cpu, *mini_ram, *mini_bar, *mini_weather, *mini_feels;
static lv_obj_t *cpu_value, *cpu_history_title, *chart, *ram_value, *ram_pct, *ram_bar;
static lv_obj_t *swap_value, *swap_pct, *swap_bar, *server_footer;
static lv_obj_t *weather_temp, *weather_pressure, *weather_footer, *weather_title;
static lv_obj_t *weather_graph, *weather_empty, *weather_axis_min, *weather_axis_max;
static lv_obj_t *weather_mid, *weather_end, *weather_cards[2], *weather_range_label;
static struct forecast_data weather_forecast;
static bool weather_pressure_mode, graph_available, graph_stale;
static unsigned weather_hours = 48;
static int32_t graph_min, graph_max;
static lv_chart_series_t *cpu_series;
static struct weather_icon small_icon;
static lv_obj_t *page_image;
static lv_draw_buf_t *page_buffer;
static unsigned selected = PAGE_COUNT;
static uint32_t plotted_generation;
static struct {
    unsigned from, to, pending;
    uint32_t started;
    int direction;
    bool active;
} transition;

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

static int32_t forecast_value(const struct forecast_point *p, unsigned bound)
{
    if (weather_pressure_mode) return bound == 1 ? p->pl : bound == 2 ? p->ph : p->p;
    return bound == 1 ? p->lo : bound == 2 ? p->hi : p->t;
}

static int32_t graph_y(const lv_area_t *area, int32_t value)
{
    return area->y2 - (int64_t)(value - graph_min) * (area->y2 - area->y1) / (graph_max - graph_min);
}

static void forecast_draw(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(BORDER); line.width = 1;
    for (unsigned i = 0; i < 3; ++i) {
        int32_t y = area.y1 + (area.y2 - area.y1) * i / 2;
        line.p1 = (lv_point_precise_t){area.x1, y};
        line.p2 = (lv_point_precise_t){area.x2, y};
        lv_draw_line(layer, &line);
    }
    if (!graph_available) return;
    uint32_t color = weather_pressure_mode ? GOLD : CYAN;
    lv_draw_triangle_dsc_t band;
    lv_draw_triangle_dsc_init(&band);
    band.color = lv_color_hex(color);
    band.opa = graph_stale ? LV_OPA_10 : LV_OPA_20;
    line.color = lv_color_hex(color); line.width = 2;
    line.opa = graph_stale ? LV_OPA_50 : LV_OPA_COVER;
    line.round_start = line.round_end = 1;
    for (size_t i = 1; i < weather_forecast.count; ++i) {
        const struct forecast_point *a = &weather_forecast.points[i - 1], *b = &weather_forecast.points[i];
        if (b->m > (int32_t)weather_hours * 60) break;
        int32_t x1 = area.x1 + (int64_t)a->m * (area.x2 - area.x1) / (weather_hours * 60);
        int32_t x2 = area.x1 + (int64_t)b->m * (area.x2 - area.x1) / (weather_hours * 60);
        lv_point_precise_t al = {x1, graph_y(&area, forecast_value(a, 1))};
        lv_point_precise_t ah = {x1, graph_y(&area, forecast_value(a, 2))};
        lv_point_precise_t bl = {x2, graph_y(&area, forecast_value(b, 1))};
        lv_point_precise_t bh = {x2, graph_y(&area, forecast_value(b, 2))};
        band.p[0] = al; band.p[1] = bl; band.p[2] = bh; lv_draw_triangle(layer, &band);
        band.p[0] = al; band.p[1] = bh; band.p[2] = ah; lv_draw_triangle(layer, &band);
        line.p1 = (lv_point_precise_t){x1, graph_y(&area, forecast_value(a, 0))};
        line.p2 = (lv_point_precise_t){x2, graph_y(&area, forecast_value(b, 0))};
        lv_draw_line(layer, &line);
    }
}

static int32_t mmhg(int32_t pressure_dhpa)
{
    return ((int64_t)pressure_dhpa * 75006 + 500000) / 1000000;
}

static void forecast_layout(void)
{
    text(weather_title, "MODEL / %s / 90%%", weather_pressure_mode ? "PRESS" : "TEMP");
    text(weather_range_label, "%uH", weather_hours);
    text(weather_mid, "%uh", weather_hours / 2);
    text(weather_end, "%uh", weather_hours);
    for (unsigned i = 0; i < 2; ++i)
        lv_obj_set_style_border_color(weather_cards[i], lv_color_hex((i == (unsigned)weather_pressure_mode)
            ? (weather_pressure_mode ? GOLD : CYAN) : BORDER), 0);
    visible(weather_empty, !graph_available);
    if (graph_available) {
        graph_min = INT32_MAX; graph_max = INT32_MIN;
        for (size_t i = 0; i < weather_forecast.count; ++i) {
            const struct forecast_point *p = &weather_forecast.points[i];
            if (p->m > (int32_t)weather_hours * 60) break;
            int32_t low = forecast_value(p, 1), high = forecast_value(p, 2);
            if (low < graph_min) graph_min = low;
            if (high > graph_max) graph_max = high;
        }
        int32_t padding = (graph_max - graph_min) / 12;
        int32_t minimum = weather_pressure_mode ? 10 : 100;
        if (padding < minimum) padding = minimum;
        graph_min -= padding; graph_max += padding;
        text(weather_axis_min, "%d", weather_pressure_mode ? mmhg(graph_min) : graph_min / 100);
        text(weather_axis_max, "%d", weather_pressure_mode ? mmhg(graph_max) : graph_max / 100);
    } else { text(weather_axis_min, "--"); text(weather_axis_max, "--"); }
    lv_obj_invalidate(weather_graph);
}

void ui_weather_set(unsigned pressure, unsigned hours)
{
    if (pressure > 1 || (hours != 24 && hours != 48)) return;
    weather_pressure_mode = pressure != 0;
    weather_hours = hours;
    forecast_layout();
    printf("BOX3_WEATHER view=%s hours=%u points=%u\n", pressure ? "PRESS" : "TEMP", hours,
           graph_available ? (unsigned)weather_forecast.count : 0);
}

static void weather_select(lv_event_t *event)
{
    unsigned control = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    ui_weather_set(control == 2 ? weather_pressure_mode : control,
                   control == 2 ? (weather_hours == 48 ? 24 : 48) : weather_hours);
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

static void slide_page(void *var, int32_t offset)
{
    (void)var;
    lv_obj_set_x(page_image, transition.direction * PAGE_ENTER_DISTANCE * (PAGE_WIDTH - offset) / PAGE_WIDTH);
    lv_obj_set_style_image_opa(page_image, LV_OPA_90 + (LV_OPA_COVER - LV_OPA_90) * offset / PAGE_WIDTH, 0);
    int32_t marker_x = 32 + 105 * (int32_t)transition.from;
    marker_x += 105 * ((int32_t)transition.to - (int32_t)transition.from) * offset / PAGE_WIDTH;
    lv_obj_set_x(tab_marker, marker_x);
}

static void page_arrived(lv_anim_t *animation)
{
    (void)animation;
    visible(page_image, false);
    visible(pages[selected], true);
    transition.active = false;
    printf("BOX3_UI settled=%u elapsed_ms=%u\n", selected, (unsigned)lv_tick_elaps(transition.started));
    unsigned pending = transition.pending;
    transition.pending = PAGE_COUNT;
    if (pending < PAGE_COUNT && pending != selected) ui_show_page(pending);
}

void ui_show_page(unsigned page)
{
    if (page >= PAGE_COUNT) return;
    /* Keep one latest request instead of restarting or queueing animations. */
    if (transition.active) {
        transition.pending = page;
        return;
    }
    if (page == selected) return;
    unsigned previous = selected;
    selected = page;
    uint32_t started = lv_tick_get();
    const char *titles[] = {"S3 / HOME", "SERVER / SYSTEM", "OUTDOOR / WEATHER"};
    text(header, "%s", titles[page]);
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        visible(pages[i], i == page || i == previous);
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(i == page ? 0x263852 : 0x101829), 0);
        lv_obj_set_style_text_color(tab_labels[i], lv_color_hex(i == page ? CYAN : MUTED), 0);
    }
    bool snapshot = previous < PAGE_COUNT && page_buffer
        && lv_snapshot_take_to_draw_buf(pages[page], LV_COLOR_FORMAT_RGB565, page_buffer) == LV_RESULT_OK;
    if (snapshot) {
        visible(pages[previous], false);
        visible(pages[page], false);
        lv_image_set_src(page_image, page_buffer);
        visible(page_image, true);
        transition.from = previous;
        transition.to = page;
        transition.pending = PAGE_COUNT;
        transition.direction = (page + PAGE_COUNT - previous) % PAGE_COUNT == 1 ? 1 : -1;
        transition.active = true;
        transition.started = started;
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, &transition);
        lv_anim_set_exec_cb(&animation, slide_page);
        lv_anim_set_values(&animation, 0, PAGE_WIDTH);
        lv_anim_set_duration(&animation, PAGE_TRANSITION_MS);
        lv_anim_set_path_cb(&animation, lv_anim_path_custom_bezier3);
        lv_anim_set_bezier3_param(&animation, LV_BEZIER_VAL_FLOAT(0.22), LV_BEZIER_VAL_FLOAT(0.9),
                                 LV_BEZIER_VAL_FLOAT(0.3), LV_BEZIER_VAL_FLOAT(1));
        lv_anim_set_completed_cb(&animation, page_arrived);
        lv_anim_start(&animation);
    } else {
        if (previous < PAGE_COUNT) visible(pages[previous], false);
        lv_obj_set_x(tab_marker, 32 + 105 * (int32_t)page);
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
    /* Only this viewport moves; the header and navigation remain stationary. */
    lv_obj_t *viewport = box(screen, 0, 28, PAGE_WIDTH, PAGE_HEIGHT, BG, 0);
    for (int i = 0; i < PAGE_COUNT; ++i) {
        pages[i] = box(viewport, 0, 0, PAGE_WIDTH, PAGE_HEIGHT, BG, 0);
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

    for (unsigned i = 0; i < 2; ++i) {
        weather_cards[i] = card(pages[2], 10 + i * 154, 2, 146, 44);
        lv_obj_set_clickable(weather_cards[i], true);
        lv_obj_set_style_bg_color(weather_cards[i], lv_color_hex(BORDER), LV_STATE_PRESSED);
        lv_obj_add_event_cb(weather_cards[i], weather_select, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    label(weather_cards[0], 10, 4, &lv_font_montserrat_12, MUTED, "TEMPERATURE");
    weather_temp = label(weather_cards[0], 10, 17, &lv_font_montserrat_24, TEXT, "--");
    label(weather_cards[1], 10, 4, &lv_font_montserrat_12, MUTED, "PRESS / mmHg");
    weather_pressure = label(weather_cards[1], 10, 17, &lv_font_montserrat_24, TEXT, "--");
    weather_title = label(pages[2], 12, 54, &lv_font_montserrat_12, MUTED, "MODEL / TEMP / 90%");
    o = lv_button_create(pages[2]);
    lv_obj_remove_style_all(o); lv_obj_set_pos(o, 246, 49); lv_obj_set_size(o, 62, 25);
    lv_obj_set_style_radius(o, 7, 0); lv_obj_set_style_bg_color(o, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(BORDER), LV_STATE_PRESSED);
    weather_range_label = label(o, 0, 0, &lv_font_montserrat_12, TEXT, "48H");
    lv_obj_center(weather_range_label);
    lv_obj_add_event_cb(o, weather_select, LV_EVENT_CLICKED, (void *)(uintptr_t)2);
    weather_graph = group(pages[2], 44, 80, 264, 61);
    lv_obj_add_event_cb(weather_graph, forecast_draw, LV_EVENT_DRAW_MAIN, NULL);
    weather_empty = label(weather_graph, 0, 22, &lv_font_montserrat_12, MUTED, "Waiting for model");
    lv_obj_set_width(weather_empty, 264); lv_obj_set_style_text_align(weather_empty, LV_TEXT_ALIGN_CENTER, 0);
    weather_axis_max = label(pages[2], 9, 78, &lv_font_montserrat_12, MUTED, "--");
    weather_axis_min = label(pages[2], 9, 130, &lv_font_montserrat_12, MUTED, "--");
    lv_obj_set_width(weather_axis_max, 29); lv_obj_set_style_text_align(weather_axis_max, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(weather_axis_min, 29); lv_obj_set_style_text_align(weather_axis_min, LV_TEXT_ALIGN_RIGHT, 0);
    label(pages[2], 44, 144, &lv_font_montserrat_12, MUTED, "0h");
    weather_mid = label(pages[2], 165, 144, &lv_font_montserrat_12, MUTED, "24h");
    weather_end = label(pages[2], 278, 144, &lv_font_montserrat_12, MUTED, "48h");
    weather_footer = label(pages[2], 12, 161, &lv_font_montserrat_12, MUTED, "MODEL PENDING / wttr.in");
    lv_obj_set_width(weather_footer, 296); lv_label_set_long_mode(weather_footer, LV_LABEL_LONG_MODE_DOTS);
    forecast_layout();

    /* Reuse one page snapshot; live widgets resume as soon as it settles. */
    page_buffer = lv_draw_buf_create(PAGE_WIDTH, PAGE_HEIGHT, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    page_image = lv_image_create(viewport);
    lv_obj_remove_style_all(page_image);
    lv_obj_set_clickable(page_image, false);
    visible(page_image, false);

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
    tab_marker = box(nav, 32, 33, 46, 2, CYAN, 1);
    icon_set(&small_icon, false, 6, false);
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
    if (weather) {
        text(mini_weather, "%+d°", d->weather_temp_mc / 1000);
        text(mini_feels, "FEELS %+d°", d->weather_feels_mc / 1000);
        text(weather_temp, "%+d°C", d->weather_temp_mc / 1000);
    } else {
        text(mini_weather, "--"); text(mini_feels, "FEELS --"); text(weather_temp, "--");
    }
    const struct forecast_data *f = &d->forecast;
    int64_t elapsed = (now - f->received_uptime_ms) / 1000;
    int64_t forecast_age = f->age_secs + elapsed, source_age = f->source_age_secs + elapsed;
    bool available = f->ok && forecast_valid(f) && forecast_age <= 7200 && source_age <= 21600;
    bool stale = available && (forecast_age > 900 || (!link && !f->local));
    bool changed = available != graph_available || stale != graph_stale || f->count != weather_forecast.count
        || memcmp(f->points, weather_forecast.points, sizeof(f->points));
    weather_forecast = *f;
    graph_available = available; graph_stale = stale;
    if (available) {
        int32_t value = f->temp_centi < 0 ? -f->temp_centi : f->temp_centi;
        value = (value + 5) / 10;
        text(weather_temp, "%s%d.%d°C", f->temp_centi < 0 && value ? "-" : "", value/10, value%10);
        text(weather_pressure, "%d", mmhg(f->pressure_dhpa));
        text(weather_footer, "%s%s / SRC %lldm", f->local ? "USB " : "", stale ? "CACHED MODEL" : "MODEL ESTIMATE", (long long)source_age/60);
    } else {
        text(weather_pressure, "--");
        text(weather_footer, "%s", f->ok ? "MODEL STALE / waiting for source" : "MODEL PENDING / wttr.in");
    }
    lv_obj_set_style_text_color(weather_pressure, lv_color_hex(stale ? MUTED : TEXT), 0);
    if (changed) forecast_layout();
}
