#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include "diagnostics.h"
#include "provision.h"
#include "telemetry.h"

static atomic_t online;

bool radio_is_online(void)
{
    return atomic_get(&online) != 0;
}

void radio_summary(char *buf, size_t len)
{
    snprintf(buf, len, "Wi-Fi: %s | TLS: %s",
             radio_is_online() ? "connected" : "waiting",
             telemetry_sent() ? "sent" : "pending");
}

static void radio_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    provision_init();
    const struct telemetry_config *cfg;
    while (!(cfg = provision_config())) {
        k_msleep(1000);
    }
    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface) {
        printk("BOX3_WIFI no_station_interface\n");
        return;
    }
    int64_t next_attempt = 0;
    while (1) {
        bool up = net_if_is_up(iface) && net_if_is_carrier_ok(iface)
            && net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
        atomic_set(&online, up);
        if (!up && k_uptime_get() >= next_attempt) {
            struct wifi_connect_req_params req = {
                .ssid = (const uint8_t *)cfg->ssid, .ssid_length = strlen(cfg->ssid),
                .psk = (const uint8_t *)cfg->psk, .psk_length = strlen(cfg->psk),
                .security = WIFI_SECURITY_TYPE_PSK, .band = WIFI_FREQ_BAND_2_4_GHZ,
                .channel = WIFI_CHANNEL_ANY, .mfp = WIFI_MFP_OPTIONAL, .timeout = 30,
            };
            int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &req, sizeof(req));
            printk("BOX3_WIFI connect_request=%d\n", ret);
            next_attempt = k_uptime_get() + 45000;
        }
        k_msleep(1000);
    }
}
K_THREAD_DEFINE(radio_link, 4096, radio_thread, NULL, NULL, NULL, 7, 0, 1500);
