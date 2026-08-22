/*
 * net_rest.c: Tesserae REST API client. See net_rest.h.
 *
 * Transport is net_httpc (see net_httpc.h), which handles both http:// and
 * https:// origins; everything here is the Tesserae protocol on top of it --
 * building the identity/status bodies, attaching Bearer auth and If-None-Match,
 * and mapping HTTP statuses onto the outcomes the wake cycle branches on.
 */
#include "net_rest.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "net_httpc.h"
#include "config.h"
#include "sleepmgr.h"
#include "battery.h"
#include "cJSON.h"

/* Map an HTTP status to the coarse outcome the cycle reacts to. */
static rest_status_t map_status(int http)
{
    switch (http) {
        case 200: case 201: return REST_OK;
        case 204:           return REST_NO_CONTENT;
        case 304:           return REST_NOT_MODIFIED;
        case 401:           return REST_UNAUTH;
        case 403:           return REST_FORBIDDEN;
        case 404:           return REST_NOT_FOUND;
        case 429:           return REST_RATELIMIT;
        default:            return REST_HTTP_ERR;
    }
}

/* ---- core request ----
 * One JSON round trip against the configured server. `url` is a full
 * http(s)://host[:port]/path; headers[] are complete header lines (without
 * CRLF). A non-NULL body is sent as application/json.
 *
 * On success the response body is returned via *body_out as a pointer into
 * net_httpc's static buffer, valid until the next request. */
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

    http_req_t req = {
        .method = method,
        .url = url,
        .headers = headers,
        .nheaders = nh,
        .body = body,
        .timeout_ms = timeout_ms,
    };
    http_resp_t resp;
    if (!http_request(&req, &resp)) return REST_NET_ERR;

    if (etag_out && etag_cap) snprintf(etag_out, etag_cap, "%s", resp.etag);
    if (resp.status == 429 && retry_after_out) *retry_after_out = resp.retry_after_s;
    if (body_out) {
        *body_out = resp.body;
        if (body_len_out) *body_len_out = resp.body_len;
    }
    return map_status(resp.status);
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
        json_get_str(r, "device_id", out->device_id, sizeof out->device_id);
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
    json_get_str(r, "device_id", out->device_id, sizeof out->device_id);
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
    cJSON_AddNumberToObject(o, "battery_mv", battery_mv());
    cJSON_AddNumberToObject(o, "battery_pct", battery_pct());
    cJSON_AddNumberToObject(o, "rssi", rssi);
    cJSON_AddStringToObject(o, "ip", ip ? ip : "");
    cJSON_AddNumberToObject(o, "next_sleep_s", next_sleep_s);
    cJSON_AddStringToObject(o, "fw_version", fw_version);
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
    /* proto v2 status contract: tz rides every beat, echoing back whatever the
     * server last told us. Empty until it delivers one, which is the normal
     * state and not an error. */
    cJSON_AddStringToObject(o, "tz", c->tz);
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
    if (cfg) {
        out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
        json_get_str(cfg, "tz", out->tz, sizeof out->tz);
    }
    cJSON_Delete(r);
    return REST_OK;
}
