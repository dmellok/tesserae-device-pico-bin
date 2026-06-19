/*
 * net_http.c: HTTP GET into a buffer, on lwIP's HTTP client. See net_http.h.
 *
 * httpc_get_file_dns() resolves the host (name or literal IP) and streams the
 * response body to our recv callback, which copies it into the destination
 * buffer. Driven synchronously: issue the request, then poll a done flag the
 * result callback sets (threadsafe-background lwIP services it in the
 * background).
 */
#include "net_http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "lwip/apps/http_client.h"
#include "lwip/altcp.h"
#include "lwip/pbuf.h"

static uint8_t *s_dst;
static size_t   s_cap;
static size_t   s_len;
static bool     s_overflow;

static volatile bool          s_done;
static volatile httpc_result_t s_result;
static volatile u32_t          s_status;   /* HTTP status code */

static err_t recv_fn(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err)
{
    (void)arg;
    if (p == NULL) return ERR_OK;            /* connection closed; result_fn handles it */
    if (err == ERR_OK) {
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            if (s_len + q->len <= s_cap) {
                memcpy(s_dst + s_len, q->payload, q->len);
                s_len += q->len;
            } else {
                s_overflow = true;
            }
        }
        altcp_recved(conn, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void result_fn(void *arg, httpc_result_t httpc_result, u32_t rx_content_len,
                      u32_t srv_res, err_t err)
{
    (void)arg; (void)rx_content_len; (void)err;
    s_result = httpc_result;
    s_status = srv_res;
    s_done = true;
}

bool http_get(const char *url, uint8_t *dst, size_t cap, size_t *out_len,
              uint32_t timeout_ms)
{
    /* Parse http://host[:port]/path */
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    const char *colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));

    char host[128];
    size_t hlen = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (hlen >= sizeof host) hlen = sizeof host - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    uint16_t port = 80;
    if (colon) { int v = atoi(colon + 1); if (v > 0 && v < 65536) port = (uint16_t)v; }
    const char *path = slash ? slash : "/";

    s_dst = dst; s_cap = cap; s_len = 0; s_overflow = false;
    s_done = false; s_result = HTTPC_RESULT_ERR_UNKNOWN; s_status = 0;

    httpc_connection_t settings;
    memset(&settings, 0, sizeof settings);
    settings.result_fn = result_fn;

    printf("http: GET %s:%u%s\n", host, port, path);
    httpc_state_t *conn = NULL;
    cyw43_arch_lwip_begin();
    err_t e = httpc_get_file_dns(host, port, path, &settings, recv_fn, NULL, &conn);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) { printf("http: request failed to start (%d)\n", e); return false; }

    uint32_t t = 0;
    while (!s_done && t < timeout_ms) { sleep_ms(20); t += 20; }
    if (!s_done) { printf("http: timed out after %ums\n", (unsigned)t); return false; }

    bool ok = (s_result == HTTPC_RESULT_OK) && (s_status >= 200 && s_status < 300) && !s_overflow;
    if (!ok) {
        printf("http: failed (result=%d status=%lu overflow=%d, got %u bytes)\n",
               (int)s_result, (unsigned long)s_status, s_overflow, (unsigned)s_len);
        return false;
    }
    *out_len = s_len;
    printf("http: ok, %u bytes\n", (unsigned)s_len);
    return true;
}
