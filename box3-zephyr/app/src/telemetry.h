#ifndef BOX3_TELEMETRY_H
#define BOX3_TELEMETRY_H
#include <stdbool.h>
#include <stdint.h>
void telemetry_sample(int result, int32_t temperature_mc, uint32_t humidity);
bool radio_is_online(void);
bool telemetry_sent(void);
#endif
