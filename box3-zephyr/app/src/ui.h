#ifndef BOX3_UI_H
#define BOX3_UI_H
#include <stdbool.h>
#include <stdint.h>
#include "dashboard.h"
void ui_init(void);
void ui_show_page(unsigned page);
void ui_weather_set(unsigned pressure, unsigned hours);
void ui_pointer_debug(void);
void ui_update(bool room_ok, int32_t temperature_mc, uint32_t humidity,
               bool wifi, bool uploaded, const struct dashboard_data *dashboard,
               bool have_dashboard, int64_t now);
#endif
