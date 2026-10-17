#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/atomic.h>
#include "provision.h"

#define MAGIC 0x42335431
static struct telemetry_config active;
static struct telemetry_config staging;
static bool stage_open;
static atomic_t ready;
BUILD_ASSERT(sizeof(struct telemetry_config) < 3500, "Configuration must fit one NVS sector");

static bool valid(const struct telemetry_config *c)
{
    struct net_in_addr addr;
    return c->magic == MAGIC && c->interval_secs >= 60 && c->interval_secs <= 86400
        && memchr(c->ssid, 0, sizeof(c->ssid)) && strlen(c->ssid) > 0
        && memchr(c->psk, 0, sizeof(c->psk)) && strlen(c->psk) >= 8
        && memchr(c->server_ip, 0, sizeof(c->server_ip))
        && net_addr_pton(NET_AF_INET, c->server_ip, &addr) == 0
        && memchr(c->ca, 0, sizeof(c->ca)) && strstr(c->ca, "BEGIN CERTIFICATE")
        && memchr(c->cert, 0, sizeof(c->cert)) && strstr(c->cert, "BEGIN CERTIFICATE")
        && memchr(c->key, 0, sizeof(c->key)) && strstr(c->key, "BEGIN PRIVATE KEY");
}

static int load(const char *name, size_t len, settings_read_cb cb, void *arg)
{
    if (strcmp(name, "config")) {
        return -ENOENT;
    }
    if (len != sizeof(active)) {
        return -EINVAL;
    }
    int n = cb(arg, &active, sizeof(active));
    return n == sizeof(active) ? 0 : -EIO;
}
SETTINGS_STATIC_HANDLER_DEFINE(tlm_settings, "tlm", NULL, load, NULL, NULL);

void provision_init(void)
{
    int ret = settings_subsys_init();
    if (ret == 0) {
        ret = settings_load_subtree("tlm");
    }
    atomic_set(&ready, ret == 0 && valid(&active));
    printk("BOX3_CONFIG settings=%d provisioned=%d\n", ret, (int)atomic_get(&ready));
}

const struct telemetry_config *provision_config(void)
{
    return atomic_get(&ready) ? &active : NULL;
}

static int begin(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    memset(&staging, 0, sizeof(staging));
    staging.magic = MAGIC;
    staging.interval_secs = 3600;
    stage_open = true;
    shell_print(sh, "TLM_BEGIN_OK");
    return 0;
}

static int put(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    if (!stage_open) {
        return -EPERM;
    }
    char *dst = NULL;
    size_t cap = 0;
#define FIELD(n, f) if (!strcmp(argv[1], n)) { dst = staging.f; cap = sizeof(staging.f); }
    FIELD("ssid", ssid)
    FIELD("psk", psk)
    FIELD("ip", server_ip)
    FIELD("ca", ca)
    FIELD("cert", cert)
    FIELD("key", key)
#undef FIELD
    char *end;
    unsigned long offset = strtoul(argv[2], &end, 10);
    if (!dst || *end || offset != strlen(dst) || offset >= cap) {
        return -EINVAL;
    }
    uint8_t decoded[96];
    size_t n;
    int ret = base64_decode(decoded, sizeof(decoded), &n, argv[3], strlen(argv[3]));
    if (ret != 0 || n == 0 || offset + n >= cap || memchr(decoded, 0, n)) {
        return -EINVAL;
    }
    memcpy(dst + offset, decoded, n);
    dst[offset + n] = 0;
    memset(decoded, 0, sizeof(decoded));
    shell_print(sh, "TLM_CHUNK_OK");
    return 0;
}

static int interval(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    char *end;
    unsigned long n = strtoul(argv[1], &end, 10);
    if (!stage_open || *end || n < 60 || n > 86400) {
        return -EINVAL;
    }
    staging.interval_secs = n;
    shell_print(sh, "TLM_INTERVAL_OK");
    return 0;
}

static int commit(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    if (!stage_open || !valid(&staging)) {
        return -EINVAL;
    }
    int ret = settings_save_one("tlm/config", &staging, sizeof(staging));
    if (ret == 0) {
        stage_open = false;
        memset(&staging, 0, sizeof(staging));
        shell_print(sh, "TLM_COMMIT_OK");
    }
    return ret;
}

static int period(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    char *end;
    unsigned long n = strtoul(argv[1], &end, 10);
    if (stage_open || !provision_config() || *end || n < 60 || n > 86400) {
        return -EINVAL;
    }
    memcpy(&staging, &active, sizeof(staging));
    staging.interval_secs = n;
    int ret = settings_save_one("tlm/config", &staging, sizeof(staging));
    memset(&staging, 0, sizeof(staging));
    if (ret == 0) {
        shell_print(sh, "TLM_PERIOD_OK; reboot to apply");
    }
    return ret;
}

static int reboot(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "TLM_REBOOT");
    k_msleep(200);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(tlm_commands,
    SHELL_CMD_ARG(begin, NULL, "Begin local provisioning", begin, 1, 0),
    SHELL_CMD_ARG(put, NULL, "Append base64 field chunk", put, 4, 0),
    SHELL_CMD_ARG(interval, NULL, "Set seconds between uploads", interval, 2, 0),
    SHELL_CMD_ARG(commit, NULL, "Persist complete configuration", commit, 1, 0),
    SHELL_CMD_ARG(period, NULL, "Persist new upload interval; reboot to apply", period, 2, 0),
    SHELL_CMD_ARG(reboot, NULL, "Restart and apply configuration", reboot, 1, 0),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(tlm, &tlm_commands, "Local USB provisioning; no secret readback", NULL);
