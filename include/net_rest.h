/*
 * net_rest.h: Tesserae REST API client (transport alternative to MQTT).
 *
 * A small raw-TCP HTTP/1.1 client that can do what net_http's lwIP httpc cannot:
 * set request headers (Authorization, If-None-Match, X-Pairing-Code) and read
 * the response status code + selected response headers (ETag, Retry-After).
 * Used for the JSON control endpoints under <server_url>/api/v1/device/ . The
 * large frame .bin is still fetched with net_http's http_get().
 *
 * The wrappers read identity from config (config_device_id(), server_url,
 * device_token, pairing_code, last_frame_etag); per-cycle values (panel size,
 * mac, rssi, ip) are passed in. Single request in flight at a time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Outcome of a REST call, mapping the HTTP statuses the cycle reacts to. */
typedef enum {
    REST_OK = 0,          /* 200 / 201 */
    REST_NOT_MODIFIED,    /* 304 (frame unchanged) */
    REST_NO_CONTENT,      /* 204 (nothing rendered server-side yet) */
    REST_UNAUTH,          /* 401 (token invalid/revoked) */
    REST_FORBIDDEN,       /* 403 (pairing code invalid/expired) */
    REST_RATELIMIT,       /* 429 (retry_after_s populated) */
    REST_HTTP_ERR,        /* other 4xx/5xx */
    REST_NET_ERR,         /* DNS / TCP / timeout */
} rest_status_t;

typedef struct {
    char     token[256];        /* device_token to persist */
    char     device_id[33];     /* canonical id the server matched us to (MAC) */
    int32_t  sleep_interval_s;  /* from config object, -1 if absent */
    uint32_t server_time;       /* unix seconds, 0 if absent */
    int      retry_after_s;     /* set on REST_RATELIMIT */
} rest_register_out_t;

typedef struct {
    bool     registered;        /* admin clicked Register: token is present */
    char     token[256];        /* device_token, when registered */
    char     device_id[33];     /* canonical id the server matched us to (MAC) */
    int32_t  sleep_interval_s;  /* from config object, -1 if absent */
    uint32_t server_time;       /* unix seconds, 0 if absent */
    int      retry_after_s;     /* how long to wait before the next discover */
} rest_discover_out_t;

typedef struct {
    char     url[256];          /* frame .bin URL (may be relative) */
    char     format[16];        /* e.g. "bin" */
    uint16_t panel_w, panel_h;
    char     etag[80];          /* new ETag to persist (quotes stripped) */
} rest_frame_out_t;

typedef struct {
    int32_t  next_poll_s;       /* deep-sleep duration to use, -1 if absent */
    int32_t  sleep_interval_s;  /* from config object, -1 if absent */
    uint32_t server_time;       /* unix seconds, 0 if absent */
    int      retry_after_s;     /* set on REST_RATELIMIT */
} rest_status_out_t;

/* POST /api/v1/device/discover (unauthenticated). The admin claims the device by
 * clicking Register in the Tesserae UI; the next discover returns the token by
 * MAC match. REST_OK means the call succeeded: inspect out->registered (true =
 * token claimed; false = waiting on the admin, sleep out->retry_after_s). */
rest_status_t rest_discover(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_discover_out_t *out, uint32_t timeout_ms);

/* POST /api/v1/device/register with the X-Pairing-Code header (opt-in strict
 * gating). Idempotent on the server (a retry returns the existing token). On
 * REST_OK, out->token holds the device token to persist. */
rest_status_t rest_register(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_register_out_t *out, uint32_t timeout_ms);

/* GET /api/v1/device/<id>/frame with Bearer auth and If-None-Match (from the
 * cached etag). REST_OK fills out (incl. the new etag); REST_NOT_MODIFIED and
 * REST_NO_CONTENT mean skip the paint. */
rest_status_t rest_get_frame(rest_frame_out_t *out, uint32_t timeout_ms);

/* POST /api/v1/device/<id>/status with Bearer auth. Reports telemetry and reads
 * back next_poll_s / config / server_time. */
rest_status_t rest_post_status(int rssi, const char *ip,
                               uint16_t panel_w, uint16_t panel_h,
                               int32_t next_sleep_s, uint32_t sleep_until,
                               const char *fw_version,
                               rest_status_out_t *out, uint32_t timeout_ms);
