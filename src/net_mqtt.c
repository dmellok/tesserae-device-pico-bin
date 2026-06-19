/*
 * net_mqtt.c: Tesserae MQTT client over lwIP's MQTT app. See net_mqtt.h.
 *
 * lwIP's MQTT is callback-driven on the raw API; here we drive it synchronously
 * by issuing the call (under cyw43_arch_lwip_begin/end) then polling a volatile
 * flag that the callback sets. Threadsafe-background cyw43_arch services lwIP in
 * the background, so sleep_ms() between checks is enough.
 */
#include "net_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#define CLIENT_ID       "tesserae-pico-bin-1"
#define DEFAULT_PORT    1883
#define DEFAULT_DEVID   "pico"
#define PAYLOAD_MAX     512

/* ----- shared connection state (one operation at a time per boot) ----- */
static mqtt_client_t *s_client;

static volatile bool s_conn_done;
static volatile mqtt_connection_status_t s_conn_status;

static char s_topic_frame[96];
static char s_topic_config[96];
static char s_topic_status[96];

static volatile int    s_cur;            /* 1=frame, 2=config, 0=other, while receiving */
static char            s_payload[PAYLOAD_MAX];
static volatile size_t s_payload_len;
static volatile int    s_msg_topic;      /* set when a full message is ready */
static volatile bool   s_msg_ready;

static volatile bool  s_pub_done;
static volatile err_t s_pub_err;

static volatile bool  s_dns_done, s_dns_ok;
static ip_addr_t      s_dns_ip;

/* ----- small helpers ----- */

static bool wait_flag(volatile bool *flag, uint32_t timeout_ms)
{
    uint32_t t = 0;
    while (!*flag && t < timeout_ms) { sleep_ms(10); t += 10; }
    return *flag;
}

/* Parse "mqtt://host:port" / "host:port" / "host" into host + port. */
static void parse_uri(const char *uri, char *host, size_t hostcap, uint16_t *port)
{
    *port = DEFAULT_PORT;
    const char *p = strstr(uri, "://");
    p = p ? p + 3 : uri;
    const char *colon = strrchr(p, ':');
    size_t hlen = colon ? (size_t)(colon - p) : strlen(p);
    if (hlen >= hostcap) hlen = hostcap - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    if (colon) { int v = atoi(colon + 1); if (v > 0 && v < 65536) *port = (uint16_t)v; }
}

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name; (void)arg;
    if (ipaddr) { s_dns_ip = *ipaddr; s_dns_ok = true; }
    s_dns_done = true;
}

static bool resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms)
{
    if (ip4addr_aton(host, ip_2_ip4(out))) { IP_SET_TYPE(out, IPADDR_TYPE_V4); return true; }
    s_dns_done = false; s_dns_ok = false;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &s_dns_ip, dns_cb, NULL);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) { *out = s_dns_ip; return true; }      /* was cached */
    if (e == ERR_INPROGRESS && wait_flag(&s_dns_done, timeout_ms) && s_dns_ok) {
        *out = s_dns_ip; return true;
    }
    return false;
}

/* extract a URL: bare http(s) string, or the value of a "url" JSON field. */
static bool extract_url(const char *buf, char *out, size_t cap)
{
    if (strncmp(buf, "http://", 7) == 0 || strncmp(buf, "https://", 8) == 0) {
        snprintf(out, cap, "%s", buf);
        char *nl = strpbrk(out, "\r\n\""); if (nl) *nl = '\0';
        return out[0] != '\0';
    }
    const char *k = strstr(buf, "\"url\"");
    if (!k) return false;
    const char *q = strchr(k + 5, '"');          /* opening quote of value */
    if (!q) return false;
    q++;
    const char *e = strchr(q, '"');
    if (!e) return false;
    size_t n = (size_t)(e - q);
    if (n >= cap) n = cap - 1;
    memcpy(out, q, n); out[n] = '\0';
    return n > 0;
}

static bool extract_int(const char *buf, const char *key, int32_t *out)
{
    const char *k = strstr(buf, key);
    if (!k) return false;
    const char *c = strchr(k, ':');
    if (!c) return false;
    *out = (int32_t)atoi(c + 1);
    return true;
}

/* ----- MQTT callbacks ----- */

static void conn_cb(mqtt_client_t *c, void *arg, mqtt_connection_status_t status)
{
    (void)c; (void)arg;
    s_conn_status = status;
    s_conn_done = true;
}

static void inpub_cb(void *arg, const char *topic, u32_t tot_len)
{
    (void)arg; (void)tot_len;
    if (strcmp(topic, s_topic_frame) == 0)      s_cur = 1;
    else if (strcmp(topic, s_topic_config) == 0) s_cur = 2;
    else                                         s_cur = 0;
    s_payload_len = 0;
}

static void indata_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    (void)arg;
    if (s_cur != 0 && s_payload_len + len < sizeof s_payload) {
        memcpy(s_payload + s_payload_len, data, len);
        s_payload_len += len;
    }
    if (flags & MQTT_DATA_FLAG_LAST) {
        s_payload[s_payload_len] = '\0';
        s_msg_topic = s_cur;
        s_msg_ready = true;
    }
}

static void sub_cb(void *arg, err_t err) { (void)arg; (void)err; }

static void pub_cb(void *arg, err_t err) { (void)arg; s_pub_err = err; s_pub_done = true; }

/* ----- connect/disconnect shared by both operations ----- */

static void build_topics(const char *device_id)
{
    const char *id = (device_id && device_id[0]) ? device_id : DEFAULT_DEVID;
    snprintf(s_topic_frame,  sizeof s_topic_frame,  "tesserae/%s/frame/bin", id);
    snprintf(s_topic_config, sizeof s_topic_config, "tesserae/%s/config", id);
    snprintf(s_topic_status, sizeof s_topic_status, "tesserae/%s/status", id);
}

static bool connect_broker(const char *uri, const char *user, const char *pass,
                           uint32_t timeout_ms)
{
    char host[128]; uint16_t port;
    parse_uri(uri, host, sizeof host, &port);

    ip_addr_t ip;
    if (!resolve(host, &ip, timeout_ms)) {
        printf("mqtt: dns failed for '%s'\n", host);
        return false;
    }
    printf("mqtt: connecting to %s:%u\n", ipaddr_ntoa(&ip), port);

    if (s_client == NULL) s_client = mqtt_client_new();
    if (s_client == NULL) { printf("mqtt: client alloc failed\n"); return false; }

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof ci);
    ci.client_id   = CLIENT_ID;
    ci.client_user = (user && user[0]) ? user : NULL;
    ci.client_pass = (pass && pass[0]) ? pass : NULL;
    ci.keep_alive  = 30;
    ci.will_topic  = s_topic_status;
    ci.will_msg    = "{\"state\":\"offline\"}";
    ci.will_qos    = 1;
    ci.will_retain = 0;   /* non-retained, so it never clobbers the retained status */

    s_conn_done = false;
    mqtt_set_inpub_callback(s_client, inpub_cb, indata_cb, NULL);
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_connect(s_client, &ip, port, conn_cb, NULL, &ci);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) { printf("mqtt: connect call failed (%d)\n", e); return false; }

    if (!wait_flag(&s_conn_done, timeout_ms) || s_conn_status != MQTT_CONNECT_ACCEPTED) {
        printf("mqtt: connect rejected/timed out (status=%d)\n", (int)s_conn_status);
        return false;
    }
    return true;
}

static void disconnect_broker(void)
{
    if (s_client) {
        cyw43_arch_lwip_begin();
        mqtt_disconnect(s_client);
        cyw43_arch_lwip_end();
    }
}

/* ----- public API ----- */

bool mqtt_fetch_retained(const char *uri, const char *device_id,
                         const char *user, const char *pass,
                         char *url_out, size_t url_cap,
                         int32_t *sleep_out, uint32_t timeout_ms)
{
    url_out[0] = '\0';
    build_topics(device_id);
    if (!connect_broker(uri, user, pass, timeout_ms)) return false;

    s_msg_ready = false;
    cyw43_arch_lwip_begin();
    mqtt_subscribe(s_client, s_topic_frame, 1, sub_cb, NULL);
    mqtt_subscribe(s_client, s_topic_config, 1, sub_cb, NULL);
    cyw43_arch_lwip_end();

    bool got_url = false;
    uint32_t t = 0;
    uint32_t grace = 0;   /* once we have the URL, linger briefly for config */
    while (t < timeout_ms) {
        if (s_msg_ready) {
            int topic = s_msg_topic;
            s_msg_ready = false;
            if (topic == 1) {
                if (extract_url(s_payload, url_out, url_cap)) {
                    printf("mqtt: frame url = %s\n", url_out);
                    got_url = true;
                }
            } else if (topic == 2) {
                int32_t s;
                if (sleep_out && extract_int(s_payload, "sleep_interval_s", &s)) {
                    *sleep_out = s;
                    printf("mqtt: config sleep_interval_s = %ld\n", (long)s);
                }
            }
        }
        if (got_url) { grace += 10; if (grace >= 300) break; }
        sleep_ms(10);
        t += 10;
    }

    disconnect_broker();
    return got_url;
}

bool mqtt_publish_status(const char *uri, const char *device_id,
                         const char *user, const char *pass,
                         const char *json, uint32_t timeout_ms)
{
    build_topics(device_id);
    if (!connect_broker(uri, user, pass, timeout_ms)) return false;

    s_pub_done = false;
    cyw43_arch_lwip_begin();
    err_t e = mqtt_publish(s_client, s_topic_status, json, strlen(json),
                           1 /*qos*/, 1 /*retain*/, pub_cb, NULL);
    cyw43_arch_lwip_end();

    bool ok = false;
    if (e == ERR_OK && wait_flag(&s_pub_done, timeout_ms) && s_pub_err == ERR_OK) {
        printf("mqtt: status published (%u bytes)\n", (unsigned)strlen(json));
        ok = true;
    } else {
        printf("mqtt: status publish failed (call=%d, ack=%d)\n", (int)e, (int)s_pub_err);
    }

    disconnect_broker();
    return ok;
}
