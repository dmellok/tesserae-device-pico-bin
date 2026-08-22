/*
 * net_httpc.h: the firmware's one HTTP/1.1 client, over plain TCP or TLS.
 *
 * Written against lwIP's altcp abstraction, so http:// and https:// are the
 * same code path with a different pcb constructor. That is the whole reason
 * this module exists: before it, control JSON went through a hand-rolled raw-TCP
 * client (headers but no TLS, small responses only) while frames went through
 * lwIP's httpc app (streaming but no request headers), and neither could reach
 * an https:// cloud relay, which needs both at once.
 *
 * Two response sinks, because the two response shapes have nothing in common:
 *
 *   - No sink (sink == NULL): the body accumulates in a small internal buffer
 *     and comes back NUL-terminated via resp->body. For control JSON.
 *   - Caller sink: the body streams straight into the caller's buffer, which
 *     for a 13.3" frame is a ~960 KB window in PSRAM. Nothing is copied twice
 *     and nothing is held in SRAM.
 *
 * Both Content-Length and chunked transfer-encoding are handled; a Cloudflare
 * Worker (which is what a cloud relay is) will use chunked whenever it streams,
 * and feeding chunk framing into a frame buffer paints noise.
 *
 * One request in flight at a time -- the whole firmware is a single sequential
 * cycle, and the state lives in statics.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *method;            /* "GET" / "POST" */
    const char *url;               /* http://host[:port]/path or https://... */
    const char *const *headers;    /* complete header lines, WITHOUT CRLF */
    int         nheaders;
    const char *body;              /* request body, NULL for none */
    const char *content_type;      /* NULL defaults to application/json */
    uint32_t    timeout_ms;

    /* Streaming sink for the response body. NULL uses the internal buffer. */
    uint8_t    *sink;
    size_t      sink_cap;
} http_req_t;

typedef struct {
    int         status;         /* HTTP status; 0 when nothing was parsed */
    char        etag[80];       /* ETag with surrounding quotes stripped, "" if none */
    int         retry_after_s;  /* Retry-After, 0 if absent */
    uint32_t    date_epoch;     /* Date header as unix seconds, 0 if unparseable */
    const char *body;           /* internal-buffer mode only; NUL-terminated */
    size_t      body_len;       /* bytes of body received (either mode) */
    bool        truncated;      /* the body did not fit the sink/buffer */
} http_resp_t;

/* Perform one request. Returns false only on a TRANSPORT failure (DNS, connect,
 * TLS handshake, timeout) -- an HTTP error status is a successful request with
 * a status to inspect, since 304 and 404 are meaningful answers here, not
 * errors. WiFi must be up.
 *
 * resp->body points into a static buffer valid until the next call. */
bool http_request(const http_req_t *req, http_resp_t *resp);

/* Convenience wrapper kept for the frame-download path: GET `url` into dst, up
 * to cap bytes. True only on a 2xx whose body fit entirely in cap. */
bool http_get(const char *url, uint8_t *dst, size_t cap, size_t *out_len,
              uint32_t timeout_ms);
