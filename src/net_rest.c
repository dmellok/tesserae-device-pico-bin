/*
 * net_rest.c: Tesserae REST API client. See net_rest.h.
 *
 * A minimal synchronous HTTP/1.1 client on lwIP's raw TCP API (the httpc app
 * used by net_http.c cannot set request headers or expose response status +
 * headers, which the REST protocol needs). One request at a time: resolve the
 * host, connect, write the request, accumulate the whole response into a static
 * buffer until the server closes the connection, then parse status + headers +
 * body. lwIP runs in threadsafe-background mode, so API calls from this (main)
 * thread are wrapped in cyw43_arch_lwip_begin/end; the tcp callbacks already run
 * under that lock.
 */
#include "net_rest.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#include "config.h"
#include "sleepmgr.h"
#include "cJSON.h"

#define REST_RX_MAX   4096          /* JSON control responses are small */
#define REST_REQ_MAX  1408          /* request line + headers + body */

/* ---- in-flight request state (single request at a time) ---- */
static char            s_req[REST_REQ_MAX];
static size_t          s_req_len;
static size_t          s_req_sent;

static char            s_rx[REST_RX_MAX + 1];
static volatile size_t s_rx_len;
static volatile bool   s_overflow;

static struct tcp_pcb *s_pcb;
static volatile bool   s_pcb_dead;   /* lwIP freed the pcb (err callback) */
static volatile bool   s_done;
static volatile bool   s_err;

static ip_addr_t       s_ip;
static volatile int    s_dns;        /* 0 pending, 1 ok, 2 fail */

/* ---- raw TCP callbacks (run under the lwIP lock) ---- */

static void try_send(struct tcp_pcb *pcb)
{
    while (s_req_sent < s_req_len) {
        u16_t avail = tcp_sndbuf(pcb);
        if (avail == 0) break;
        size_t remain = s_req_len - s_req_sent;
        u16_t n = (remain < avail) ? (u16_t)remain : avail;
        if (tcp_write(pcb, s_req + s_req_sent, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        s_req_sent += n;
    }
    tcp_output(pcb);
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)len;
    try_send(pcb);
    return ERR_OK;
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (p == NULL) { s_done = true; return ERR_OK; }   /* server closed: complete */
    if (err == ERR_OK) {
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            if (s_rx_len + q->len <= REST_RX_MAX) {
                memcpy(s_rx + s_rx_len, q->payload, q->len);
                s_rx_len += q->len;
            } else {
                s_overflow = true;
            }
        }
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
    (void)arg; (void)err;
    s_pcb = NULL;          /* lwIP has freed it */
    s_pcb_dead = true;
    s_err = true;
    s_done = true;
}

static err_t connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { s_err = true; s_done = true; return ERR_OK; }
    s_req_sent = 0;
    try_send(pcb);
    return ERR_OK;
}

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name; (void)arg;
    if (ipaddr) { s_ip = *ipaddr; s_dns = 1; }
    else        { s_dns = 2; }
}

/* ---- response parsing ---- */

static int parse_status(void)
{
    if (s_rx_len < 12 || strncmp(s_rx, "HTTP/", 5) != 0) return 0;
    const char *sp = memchr(s_rx, ' ', s_rx_len);
    return sp ? atoi(sp + 1) : 0;
}

/* Case-insensitive header lookup over the header block (up to the blank line).
 * Copies the trimmed value into out. Returns true if found. */
static bool header_value(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    const char *end = strstr(s_rx, "\r\n\r\n");
    const char *hdr_end = end ? end : (s_rx + s_rx_len);
    size_t nlen = strlen(name);

    const char *nl = memchr(s_rx, '\n', (size_t)(hdr_end - s_rx));
    const char *line = nl ? nl + 1 : hdr_end;   /* skip the status line */
    while (line < hdr_end) {
        const char *eol = memchr(line, '\n', (size_t)(hdr_end - line));
        const char *lend = eol ? eol : hdr_end;
        if ((size_t)(lend - line) > nlen &&
            strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (v < lend && (*v == ' ' || *v == '\t')) v++;
            size_t n = 0;
            while (v < lend && *v != '\r' && *v != '\n' && n < cap - 1) out[n++] = *v++;
            out[n] = '\0';
            return true;
        }
        if (!eol) break;
        line = eol + 1;
    }
    return false;
}

static void parse_etag(char *out, size_t cap)
{
    char raw[96];
    if (!header_value("ETag", raw, sizeof raw)) { out[0] = '\0'; return; }
    const char *v = raw;
    size_t len = strlen(v);
    if (len >= 2 && v[0] == '"' && v[len - 1] == '"') { v++; len -= 2; }   /* strip quotes */
    if (len >= cap) len = cap - 1;
    memcpy(out, v, len);
    out[len] = '\0';
}

/* Days since the Unix epoch for a civil date (Howard Hinnant's algorithm). */
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

/* Parse an RFC 1123 HTTP Date ("Sun, 06 Nov 1994 08:49:37 GMT") to a Unix epoch.
 * Returns 0 if it does not look like a plausible recent timestamp. The server's
 * Date header is our authoritative LAN clock source (SNTP needs internet). */
static uint32_t parse_http_date(const char *v)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *comma = strchr(v, ',');
    const char *p = comma ? comma + 1 : v;
    while (*p == ' ') p++;

    int d = 0, y = 0, hh = 0, mm = 0, ss = 0; char mon[4] = {0};
    if (sscanf(p, "%d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return 0;
    const char *mp = strstr(months, mon);
    if (!mp || y < 2020 || y > 2100) return 0;
    unsigned m = (unsigned)((mp - months) / 3) + 1u;

    long long e = (long long)days_from_civil(y, m, (unsigned)d) * 86400LL
                  + hh * 3600 + mm * 60 + ss;
    return (e > 1500000000LL) ? (uint32_t)e : 0;   /* sanity: past ~2017 */
}

/* Map an HTTP status to the coarse outcome the cycle reacts to. */
static rest_status_t map_status(int http)
{
    switch (http) {
        case 200: case 201: return REST_OK;
        case 204:           return REST_NO_CONTENT;
        case 304:           return REST_NOT_MODIFIED;
        case 401:           return REST_UNAUTH;
        case 403:           return REST_FORBIDDEN;
        case 429:           return REST_RATELIMIT;
        default:            return REST_HTTP_ERR;
    }
}

/* ---- core request ----
 * url is a full http://host[:port]/path. headers[] are complete header lines
 * (without CRLF). A non-NULL body is sent as application/json with an
 * auto-added Content-Length. On REST_OK the body string is returned via
 * *body_out (a pointer into the static rx buffer, valid until the next call). */
static rest_status_t do_request(const char *method, const char *url,
                                const char *const *headers, int nh,
                                const char *body,
                                char *etag_out, size_t etag_cap,
                                int *retry_after_out,
                                const char **body_out, size_t *body_len_out,
                                uint32_t timeout_ms)
{
    if (retry_after_out) *retry_after_out = 0;
    if (body_out)        *body_out = NULL;
    if (body_len_out)    *body_len_out = 0;
    if (etag_out && etag_cap) etag_out[0] = '\0';

    /* Parse http://host[:port]/path (mirrors net_http.c). */
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    const char *colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
    char host[128];
    size_t hlen = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (hlen >= sizeof host) hlen = sizeof host - 1;
    memcpy(host, p, hlen); host[hlen] = '\0';
    uint16_t port = 80;
    if (colon) { int v = atoi(colon + 1); if (v > 0 && v < 65536) port = (uint16_t)v; }
    const char *path = slash ? slash : "/";

    /* Build the request. */
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(s_req, sizeof s_req,
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n",
                     method, path, host, port);
    for (int i = 0; i < nh && n > 0 && n < (int)sizeof s_req; i++)
        n += snprintf(s_req + n, sizeof s_req - n, "%s\r\n", headers[i]);
    if (body && n > 0 && n < (int)sizeof s_req)
        n += snprintf(s_req + n, sizeof s_req - n,
                      "Content-Type: application/json\r\nContent-Length: %u\r\n",
                      (unsigned)blen);
    if (n > 0 && n < (int)sizeof s_req)
        n += snprintf(s_req + n, sizeof s_req - n, "\r\n");
    if (n <= 0 || n >= (int)sizeof s_req - (int)blen) {
        printf("rest: request too large\n");
        return REST_NET_ERR;
    }
    if (body) { memcpy(s_req + n, body, blen); n += (int)blen; }
    s_req_len = (size_t)n;

    /* Reset state. */
    s_rx_len = 0; s_overflow = false; s_done = false; s_err = false;
    s_pcb_dead = false; s_dns = 0;

    /* Resolve the host. */
    cyw43_arch_lwip_begin();
    err_t de = dns_gethostbyname(host, &s_ip, dns_cb, NULL);
    cyw43_arch_lwip_end();
    if (de == ERR_OK) s_dns = 1;
    else if (de != ERR_INPROGRESS) { printf("rest: dns start failed (%d)\n", de); return REST_NET_ERR; }
    uint32_t t = 0;
    while (s_dns == 0 && t < timeout_ms) { sleep_ms(20); t += 20; }
    if (s_dns != 1) { printf("rest: dns failed for %s\n", host); return REST_NET_ERR; }

    /* Connect + drive the exchange. */
    cyw43_arch_lwip_begin();
    s_pcb = tcp_new_ip_type(IP_GET_TYPE(&s_ip));
    if (s_pcb) {
        tcp_err(s_pcb, err_cb);
        tcp_recv(s_pcb, recv_cb);
        tcp_sent(s_pcb, sent_cb);
        if (tcp_connect(s_pcb, &s_ip, port, connected_cb) != ERR_OK) {
            tcp_abort(s_pcb); s_pcb = NULL; s_pcb_dead = true;
        }
    }
    cyw43_arch_lwip_end();
    if (s_pcb == NULL && s_pcb_dead) { printf("rest: connect failed\n"); return REST_NET_ERR; }

    printf("rest: %s %s:%u%s\n", method, host, port, path);
    while (!s_done && t < timeout_ms) { sleep_ms(20); t += 20; }

    /* Tear down (unless lwIP already freed it via err_cb). */
    cyw43_arch_lwip_begin();
    if (s_pcb && !s_pcb_dead) {
        tcp_recv(s_pcb, NULL); tcp_sent(s_pcb, NULL); tcp_err(s_pcb, NULL);
        if (tcp_close(s_pcb) != ERR_OK) tcp_abort(s_pcb);
        s_pcb = NULL;
    }
    cyw43_arch_lwip_end();

    if (!s_done) { printf("rest: timeout after %ums\n", (unsigned)t); return REST_NET_ERR; }
    if (s_err || s_rx_len == 0) { printf("rest: transport error\n"); return REST_NET_ERR; }
    if (s_overflow) printf("rest: response truncated at %d bytes\n", REST_RX_MAX);

    s_rx[s_rx_len] = '\0';
    int http = parse_status();
    if (etag_out && etag_cap) parse_etag(etag_out, etag_cap);

    if (http == 429 && retry_after_out) {
        char ra[16];
        if (header_value("Retry-After", ra, sizeof ra)) *retry_after_out = atoi(ra);
    }

    /* Set the wall clock from the server's Date header (authoritative, and
     * available on every response, so it lands before the status heartbeat). */
    char date[48];
    if (header_value("Date", date, sizeof date)) {
        uint32_t e = parse_http_date(date);
        if (e) sleep_set_epoch(e);
    }

    const char *bend = strstr(s_rx, "\r\n\r\n");
    if (bend && body_out) {
        *body_out = bend + 4;
        if (body_len_out) *body_len_out = s_rx_len - (size_t)((bend + 4) - s_rx);
    }
    printf("rest: <- %d (%u bytes)\n", http, (unsigned)s_rx_len);
    return map_status(http);
}

/* ---- small JSON helpers ---- */

static void json_get_str(const cJSON *o, const char *k, char *out, size_t cap)
{
    out[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
}

static int32_t json_get_int(const cJSON *o, const char *k, int32_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : dflt;
}

/* ---- typed wrappers ---- */

/* The identity body shared by /discover and /register. Caller frees. */
static char *identity_body(uint16_t panel_w, uint16_t panel_h,
                           const char *mac, const char *fw_version)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "device_id", config_device_id());
    cJSON_AddStringToObject(o, "kind", "pico_bin_client");
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
    cJSON_AddStringToObject(o, "fw_version", fw_version);
    cJSON_AddStringToObject(o, "mac", mac ? mac : "");
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return body;
}

rest_status_t rest_discover(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_discover_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->sleep_interval_s = -1;
    out->retry_after_s = 30;

    const config_t *c = config_get();
    char url[200];
    snprintf(url, sizeof url, "%s/api/v1/device/discover", c->server_url);

    char *body = identity_body(panel_w, panel_h, mac, fw_version);
    if (!body) return REST_NET_ERR;

    const char *rbody = NULL; size_t rlen = 0;
    rest_status_t st = do_request("POST", url, NULL, 0, body, NULL, 0,
                                  &out->retry_after_s, &rbody, &rlen, timeout_ms);
    free(body);
    if (st != REST_OK) return st;   /* 429 (retry_after from header) / errors */

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    out->registered  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "registered"));
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    int32_t ra = json_get_int(r, "retry_after_s", 30);
    out->retry_after_s = (ra > 0) ? (int)ra : 30;
    if (out->registered) {
        json_get_str(r, "device_token", out->token, sizeof out->token);
        cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
        if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    }
    cJSON_Delete(r);
    if (out->registered && out->token[0] == '\0') return REST_HTTP_ERR;
    return REST_OK;
}

rest_status_t rest_register(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_register_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->sleep_interval_s = -1;

    const config_t *c = config_get();
    char url[200];
    snprintf(url, sizeof url, "%s/api/v1/device/register", c->server_url);

    char *body = identity_body(panel_w, panel_h, mac, fw_version);
    if (!body) return REST_NET_ERR;

    char pc[64];
    snprintf(pc, sizeof pc, "X-Pairing-Code: %s", c->pairing_code);
    const char *hdrs[] = { pc };

    const char *rbody = NULL; size_t rlen = 0;
    rest_status_t st = do_request("POST", url, hdrs, 1, body, NULL, 0,
                                  &out->retry_after_s, &rbody, &rlen, timeout_ms);
    free(body);
    if (st != REST_OK) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    json_get_str(r, "device_token", out->token, sizeof out->token);
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
    if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    cJSON_Delete(r);
    return out->token[0] ? REST_OK : REST_HTTP_ERR;
}

rest_status_t rest_get_frame(rest_frame_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    const config_t *c = config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/frame", c->server_url, config_device_id());

    char auth[300], inm[128];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->device_token);
    int nh = 1;
    const char *hdrs[2] = { auth, NULL };
    if (c->last_frame_etag[0]) {
        snprintf(inm, sizeof inm, "If-None-Match: \"%s\"", c->last_frame_etag);
        hdrs[1] = inm; nh = 2;
    }

    const char *rbody = NULL; size_t rlen = 0;
    rest_status_t st = do_request("GET", url, hdrs, nh, NULL,
                                  out->etag, sizeof out->etag, NULL,
                                  &rbody, &rlen, timeout_ms);
    if (st != REST_OK) return st;   /* 304 / 204 / errors handled by caller */

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    json_get_str(r, "url", out->url, sizeof out->url);
    json_get_str(r, "format", out->format, sizeof out->format);
    out->panel_w = (uint16_t)json_get_int(r, "panel_w", 0);
    out->panel_h = (uint16_t)json_get_int(r, "panel_h", 0);
    cJSON_Delete(r);
    return out->url[0] ? REST_OK : REST_HTTP_ERR;
}

rest_status_t rest_post_status(int rssi, const char *ip,
                               uint16_t panel_w, uint16_t panel_h,
                               int32_t next_sleep_s, uint32_t sleep_until,
                               const char *fw_version,
                               rest_status_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->next_poll_s = -1;
    out->sleep_interval_s = -1;

    const config_t *c = config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/status", c->server_url, config_device_id());

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "battery_mv", 0);
    cJSON_AddNumberToObject(o, "battery_pct", 0);
    cJSON_AddNumberToObject(o, "rssi", rssi);
    cJSON_AddStringToObject(o, "ip", ip ? ip : "");
    cJSON_AddNumberToObject(o, "next_sleep_s", next_sleep_s);
    cJSON_AddStringToObject(o, "fw_version", fw_version);
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
    if (sleep_until) cJSON_AddNumberToObject(o, "sleep_until", (double)sleep_until);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return REST_NET_ERR;

    char auth[300];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->device_token);
    const char *hdrs[] = { auth };

    const char *rbody = NULL; size_t rlen = 0;
    rest_status_t st = do_request("POST", url, hdrs, 1, body, NULL, 0,
                                  &out->retry_after_s, &rbody, &rlen, timeout_ms);
    free(body);
    if (st != REST_OK) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_OK;   /* 200 with an unparseable body: nothing to merge */
    out->next_poll_s = json_get_int(r, "next_poll_s", -1);
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
    if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    cJSON_Delete(r);
    return REST_OK;
}
