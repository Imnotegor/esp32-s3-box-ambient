#ifndef BOX3_PROVISION_H
#define BOX3_PROVISION_H
#include <stdint.h>
struct telemetry_config {
    uint32_t magic;
    uint32_t interval_secs;
    char ssid[33];
    char psk[65];
    char server_ip[16];
    char ca[1024];
    char cert[1024];
    char key[512];
};
void provision_init(void);
const struct telemetry_config *provision_config(void);
#endif
