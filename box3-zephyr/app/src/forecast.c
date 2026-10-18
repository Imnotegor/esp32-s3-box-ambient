#include "dashboard.h"

bool forecast_valid(const struct forecast_data *f)
{
    if (!f->ok) return f->count == 0;
    if (f->count != BOX3_FORECAST_POINTS || f->age_secs < 0 || f->source_age_secs < f->age_secs
        || f->temp_centi < -9000 || f->temp_centi > 6500
        || f->pressure_dhpa < 8500 || f->pressure_dhpa > 11000) return false;
    int32_t previous = 0;
    for (size_t i = 0; i < f->count; ++i) {
        const struct forecast_point *p = &f->points[i];
        if (p->m <= previous || p->m > 2880 || (i == 0 && p->m > 60)
            || p->t < -9000 || p->t > 6500 || p->p < 8500 || p->p > 11000
            || p->lo < -12000 || p->lo > p->t || p->hi < p->t || p->hi > 9000
            || p->pl < 6500 || p->pl > p->p || p->ph < p->p || p->ph > 13000) return false;
        previous = p->m;
    }
    return previous >= 2760;
}
