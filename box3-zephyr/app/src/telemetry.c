#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include "dashboard.h"
#include "provision.h"
#include "telemetry.h"

static K_MUTEX_DEFINE(sample_lock);
static struct {
    int32_t temperature;
    uint32_t humidity;
    int64_t taken;
    bool valid;
} sample;
static atomic_t sent;
static atomic_t send_requested;
static uint8_t recv_buf[1024];
static struct {
    int status;
    size_t length;
    char body[1025];
} reply;

static int send_now(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    atomic_set(&send_requested, 1);
    shell_print(sh, "TLM_SEND_QUEUED");
    return 0;
}
SHELL_CMD_ARG_REGISTER(box3_send, NULL, "Queue a fresh encrypted measurement", send_now, 1, 0);

void telemetry_sample(int result, int32_t temperature_mc, uint32_t humidity)
{
    k_mutex_lock(&sample_lock, K_FOREVER);
    sample.valid = result == 0;
    if (sample.valid) {
        sample.temperature = temperature_mc;
        sample.humidity = humidity;
        sample.taken = k_uptime_get();
    }
    k_mutex_unlock(&sample_lock);
}

bool telemetry_sent(void)
{
    return atomic_get(&sent) != 0;
}

static int sync_clock(void)
{
    struct sntp_time now;
    int ret = sntp_simple("162.159.200.1", 5000, &now);
    if (ret != 0 || now.seconds < 1700000000ULL) {
        return ret ? ret : -EINVAL;
    }
    struct timespec ts = {.tv_sec = now.seconds, .tv_nsec = 0};
    return sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
}

static int response(struct http_response *rsp, enum http_final_call final, void *data)
{
    ARG_UNUSED(final); ARG_UNUSED(data);
    reply.status = rsp->http_status_code;
    if (rsp->body_frag_len) {
        if (reply.length + rsp->body_frag_len > sizeof(reply.body) - 1) {
            return -ENOMEM;
        }
        memcpy(reply.body + reply.length, rsp->body_frag_start, rsp->body_frag_len);
        reply.length += rsp->body_frag_len;
        reply.body[reply.length] = 0;
    }
    return 0;
}

static int connect_tls(const struct telemetry_config *cfg)
{
    const sec_tag_t tag = 42;
    const char *hostname = cfg->server_ip;
    int verify = ZSOCK_TLS_PEER_VERIFY_REQUIRED;
    int sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TLS_1_3);
    if (sock < 0) {
        return -errno;
    }
    int ret = -EIO;
    struct zsock_timeval timeout = {.tv_sec = 8, .tv_usec = 0};
    if (zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, &tag, sizeof(tag)) < 0
        || zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME, hostname, strlen(hostname) + 1) < 0
        || zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_PEER_VERIFY, &verify, sizeof(verify)) < 0
        || zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0
        || zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        ret = -errno;
        goto failed;
    }
    struct net_sockaddr_in addr = {.sin_family = NET_AF_INET, .sin_port = net_htons(443)};
    if (net_addr_pton(NET_AF_INET, cfg->server_ip, &addr.sin_addr) != 0) {
        ret = -EINVAL;
        goto failed;
    }
    if (zsock_connect(sock, (struct net_sockaddr *)&addr, sizeof(addr)) < 0) {
        ret = -errno;
        goto failed;
    }
    printk("BOX3_TLS connected\n");
    return sock;
failed:
    zsock_close(sock);
    return ret;
}

static int exchange(int sock, const char *host, bool post, const char *body)
{
    memset(&reply, 0, sizeof(reply));
    const char *headers[] = {"Connection: keep-alive\r\n", NULL};
    struct http_request req = {
        .method = post ? HTTP_POST : HTTP_GET,
        .url = post ? "/v1/measurements" : "/v1/dashboard",
        .host = host, .protocol = "HTTP/1.1",
        .content_type_value = post ? "application/json" : NULL,
        .payload = body, .payload_len = post ? 512 : 0,
        .optional_headers = headers,
        .response = response, .recv_buf = recv_buf, .recv_buf_len = sizeof(recv_buf),
    };
    int ret = http_client_req(sock, &req, 8000, NULL);
    if (ret >= 0) {
        ret = reply.status == (post ? 204 : 200) ? 0 : -EPROTO;
    }
    if (ret == 0 && !post) {
        ret = dashboard_accept(reply.body, reply.length);
    }
    printk(post ? "BOX3_HTTPS result=%d status=%d\n" : "BOX3_DASH result=%d status=%d\n",
           ret, reply.status);
    return ret;
}

static bool make_payload(char *body, size_t size, const uint32_t *boot,
                         unsigned long long sequence, uint32_t interval)
{
    int64_t now = k_uptime_get();
    k_mutex_lock(&sample_lock, K_FOREVER);
    bool fresh = sample.valid && now - sample.taken <= 15000;
    int32_t temperature = sample.temperature;
    uint32_t humidity = sample.humidity;
    int64_t age = now - sample.taken;
    k_mutex_unlock(&sample_lock);
    if (!fresh) {
        return false;
    }
    memset(body, ' ', size - 1);
    int n = snprintf(body, size,
        "{\"node_id\":\"box3\",\"boot_id\":\"%08x%08x\",\"sequence\":%llu,"
        "\"temperature_mc\":%d,\"humidity_milli_percent\":%u,"
        "\"sample_age_ms\":%lld,\"uptime_ms\":%lld,\"interval_secs\":%u}",
        boot[0], boot[1], sequence, temperature, humidity,
        (long long)age, (long long)now, interval);
    if (n < 0 || n >= size - 1) {
        return false;
    }
    body[n] = ' ';
    body[size - 1] = 0;
    return true;
}

static void sender(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    const struct telemetry_config *cfg;
    while (!(cfg = provision_config())) {
        k_msleep(1000);
    }
    int ret = tls_credential_add(42, TLS_CREDENTIAL_CA_CERTIFICATE, cfg->ca, strlen(cfg->ca) + 1);
    if (ret == 0) {
        ret = tls_credential_add(42, TLS_CREDENTIAL_PUBLIC_CERTIFICATE, cfg->cert, strlen(cfg->cert) + 1);
    }
    if (ret == 0) {
        ret = tls_credential_add(42, TLS_CREDENTIAL_PRIVATE_KEY, cfg->key, strlen(cfg->key) + 1);
    }
    if (ret != 0) {
        printk("BOX3_HTTPS credential_error=%d\n", ret);
        return;
    }
    uint32_t boot[2];
    sys_rand_get(boot, sizeof(boot));
    unsigned long long sequence = 0;
    bool clock_ready = false;
    unsigned int upload_backoff = 15, poll_backoff = 5;
    int64_t next_upload = 0, next_poll = 0;
    int sock = -1;
    while (1) {
        int64_t now = k_uptime_get();
        if (!radio_is_online()) {
            if (sock >= 0) {
                zsock_close(sock);
                sock = -1;
            }
            k_msleep(1000);
            continue;
        }
        bool post = now >= next_upload || atomic_get(&send_requested);
        bool poll = now >= next_poll;
        if (!post && !poll) {
            k_msleep(100);
            continue;
        }
        atomic_clear(&send_requested);
        if (!clock_ready) {
            ret = sync_clock();
            printk("BOX3_HTTPS clock_sync=%d\n", ret);
            if (ret != 0) {
                next_upload = next_poll = k_uptime_get() + 15000;
                continue;
            }
            clock_ready = true;
        }
        if (sock < 0) {
            sock = connect_tls(cfg);
        }
        if (sock < 0) {
            ret = sock;
        } else {
            ret = 0;
            if (post) {
                char body[513];
                if (make_payload(body, sizeof(body), boot, ++sequence, cfg->interval_secs)) {
                    ret = exchange(sock, cfg->server_ip, true, body);
                    atomic_set(&sent, ret == 0);
                    next_upload = k_uptime_get() + (ret == 0 ?
                        cfg->interval_secs * 1000LL : upload_backoff * 1000LL);
                    upload_backoff = ret == 0 ? 15 : MIN(upload_backoff * 2, 300);
                } else {
                    next_upload = k_uptime_get() + 2000;
                }
            }
            if (ret == 0 && poll) {
                ret = exchange(sock, cfg->server_ip, false, NULL);
            }
        }
        if (ret != 0) {
            if (sock >= 0) {
                zsock_close(sock);
            }
            sock = -1;
            next_poll = k_uptime_get() + poll_backoff * 1000LL;
            poll_backoff = MIN(poll_backoff * 2, 30);
            if (post && k_uptime_get() >= next_upload) {
                next_upload = k_uptime_get() + upload_backoff * 1000LL;
                upload_backoff = MIN(upload_backoff * 2, 300);
            }
            printk("BOX3_TLS retry=%d\n", ret);
        } else {
            if (poll) {
                next_poll = k_uptime_get() + 5000;
            }
            poll_backoff = 5;
        }
    }
}
K_THREAD_DEFINE(telemetry_sender, 8192, sender, NULL, NULL, NULL, 8, 0, 2000);
