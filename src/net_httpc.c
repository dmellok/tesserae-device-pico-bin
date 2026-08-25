/*
 * net_httpc.c: HTTP/1.1 over altcp (plain TCP or TLS). See net_httpc.h.
 *
 * lwIP runs in threadsafe-background mode, so altcp calls made from this (main)
 * thread are wrapped in cyw43_arch_lwip_begin/end; the callbacks below already
 * run under that lock and must not take it again.
 *
 * The response is consumed incrementally rather than buffered whole: headers
 * accumulate until the blank line, then every subsequent byte goes through the
 * body sink. That is what lets a ~960 KB frame land directly in PSRAM while a
 * 200-byte JSON reply lands in a static buffer, with one implementation.
 */
#include "net_httpc.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#include "net_tls.h"
#include "sleepmgr.h"

#define REQ_MAX     1536    /* request line + headers + body */
#define HDR_MAX     2048    /* status line + response headers */
#define INLINE_MAX  4096    /* control-JSON response bodies */

/* ---- in-flight state (one request at a time) ---- */

typedef enum { CH_SIZE, CH_DATA, CH_TAIL, CH_TRAILER, CH_DONE } chunk_state_t;

static struct {
    /* request */
    char   req[REQ_MAX];
    size_t req_len, req_sent;

    /* response headers */
    char   hdr[HDR_MAX + 1];
    size_t hdr_len;
    bool   hdr_done;

    /* response body */
    uint8_t *sink;          /* caller buffer, or NULL for the inline one */
    size_t   sink_cap;
    size_t   body_len;      /* bytes accepted (excludes anything dropped) */
    bool     overflow;

    /* framing */
    bool   chunked;
    bool   have_clen;
    size_t clen;
    chunk_state_t cstate;
    size_t chunk_remain;
    char   sizebuf[24];
    size_t sizelen;

    /* transport */
    struct altcp_pcb *pcb;
    volatile bool pcb_dead;   /* lwIP freed it via the err callback */
    volatile bool done;
    volatile bool err;
} s;

static char s_inline[INLINE_MAX + 1];

static ip_addr_t    s_ip;
static volatile int s_dns;     /* 0 pending, 1 ok, 2 fail */

/* ---- body sink ---- */

/* Progress reporting for large bodies.
 *
 * A frame download that does not finish is either SLOW or STALLED, and those
 * have opposite fixes -- a longer timeout versus an actual bug. Guessing
 * between them from a bare "timeout after 60000ms" is how you end up changing
 * the wrong thing, so report bytes against wall time and let the number say
 * which it is. Only for caller-supplied sinks: control JSON is far too small
 * to be worth a line. */
#define PROGRESS_STEP  (8u * 1024u)
static size_t   s_progress_next;
static uint32_t s_progress_start_ms;

static void sink_write(const uint8_t *p, size_t n)
{
    uint8_t *dst = s.sink ? s.sink : (uint8_t *)s_inline;
    size_t   cap = s.sink ? s.sink_cap : INLINE_MAX;

    if (s.sink != NULL && s.body_len + n >= s_progress_next) {
        uint32_t ms = (uint32_t)(time_us_64() / 1000ull) - s_progress_start_ms;
        unsigned kb = (unsigned)((s.body_len + n) / 1024u);
        printf("http: %u KB in %u ms (%u KB/s)\n", kb, (unsigned)ms,
               ms ? (unsigned)((uint64_t)kb * 1000ull / ms) : 0u);
        s_progress_next += PROGRESS_STEP;
    }
    if (s.body_len + n > cap) {
        size_t fit = (s.body_len < cap) ? (cap - s.body_len) : 0;
        if (fit) { memcpy(dst + s.body_len, p, fit); s.body_len += fit; }
        s.overflow = true;
        return;
    }
    memcpy(dst + s.body_len, p, n);
    s.body_len += n;
}

/* Feed body bytes through the chunked-transfer decoder.
 *
 * A Cloudflare Worker streams its responses, so a relay frame arrives chunked
 * far more often than not. Handing that framing straight to the panel would
 * paint hex digits and CRLFs into the image -- and, worse, the frame would be
 * the wrong LENGTH, which is the check the relay path relies on to notice
 * corruption. */
static void feed_chunked(const uint8_t *p, size_t n)
{
    while (n > 0 && s.cstate != CH_DONE) {
        switch (s.cstate) {
        case CH_SIZE:
            /* Accumulate the hex size line up to its CRLF. Chunk extensions
             * (";name=value") are legal and ignored: strtoul stops at the ';'. */
            while (n > 0 && *p != '\n') {
                if (s.sizelen < sizeof s.sizebuf - 1) s.sizebuf[s.sizelen++] = (char)*p;
                p++; n--;
            }
            if (n == 0) return;
            p++; n--;                                  /* consume the '\n' */
            s.sizebuf[s.sizelen] = '\0';
            s.chunk_remain = (size_t)strtoul(s.sizebuf, NULL, 16);
            s.sizelen = 0;
            s.cstate = (s.chunk_remain == 0) ? CH_TRAILER : CH_DATA;
            break;

        case CH_DATA: {
            size_t take = (n < s.chunk_remain) ? n : s.chunk_remain;
            sink_write(p, take);
            p += take; n -= take;
            s.chunk_remain -= take;
            if (s.chunk_remain == 0) s.cstate = CH_TAIL;
            break;
        }

        case CH_TAIL:
            /* The CRLF that closes a chunk's data. */
            while (n > 0 && *p != '\n') { p++; n--; }
            if (n == 0) return;
            p++; n--;
            s.cstate = CH_SIZE;
            break;

        case CH_TRAILER:
            /* Optional trailer headers, then a blank line. We do not need any
             * of them; the terminal chunk already means the body is complete,
             * so stop here rather than waiting for a close that a keep-alive
             * proxy might not send promptly. */
            s.cstate = CH_DONE;
            s.done = true;
            return;

        case CH_DONE:
            return;
        }
    }
    /* CH_TRAILER as well as CH_DONE: the size line for the terminal chunk can
     * be the last thing in the buffer, in which case CH_SIZE transitions to
     * CH_TRAILER and the loop exits on n == 0 without ever reaching the
     * CH_TRAILER case. Testing only for CH_DONE there would leave a fully
     * received body waiting on the timeout. */
    if (s.cstate == CH_TRAILER || s.cstate == CH_DONE) {
        s.cstate = CH_DONE;
        s.done = true;
    }
}

static void feed_body(const uint8_t *p, size_t n)
{
    if (s.chunked) { feed_chunked(p, n); return; }
    sink_write(p, n);
    /* With a known length, finish as soon as it is all here. Waiting for the
     * server's FIN instead would add a round trip to every request, and over
     * TLS it also waits on close_notify. */
    if (s.have_clen && s.body_len >= s.clen) s.done = true;
}

/* ---- header parsing ---- */

/* Case-insensitive substring search. strcasestr is a GNU extension that the
 * arm-none-eabi newlib headers do not expose, and header values are the one
 * place where case genuinely varies between servers ("chunked" vs "Chunked"). */
static bool contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return true;
    return false;
}

/* Case-insensitive lookup over the accumulated header block. */
static bool header_value(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    const char *end = strstr(s.hdr, "\r\n\r\n");
    const char *hdr_end = end ? end : (s.hdr + s.hdr_len);
    size_t nlen = strlen(name);

    const char *nl = memchr(s.hdr, '\n', (size_t)(hdr_end - s.hdr));
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

/* Called once the blank line has arrived: decide how the body is framed. */
static void headers_complete(void)
{
    s.hdr_done = true;

    char v[48];
    if (header_value("Transfer-Encoding", v, sizeof v) &&
        contains_ci(v, "chunked")) {
        s.chunked = true;
        s.cstate = CH_SIZE;
        s.sizelen = 0;
    } else if (header_value("Content-Length", v, sizeof v)) {
        s.have_clen = true;
        s.clen = (size_t)strtoul(v, NULL, 10);
        /* A declared zero-length body is complete the moment we see it: 204 and
         * 304 both come this way and would otherwise sit until the timeout. */
        if (s.clen == 0) s.done = true;
    }
}

/* Split incoming bytes between the header block and the body. */
static void feed(const uint8_t *p, size_t n)
{
    if (!s.hdr_done) {
        size_t room = HDR_MAX - s.hdr_len;
        size_t take = (n < room) ? n : room;
        memcpy(s.hdr + s.hdr_len, p, take);
        s.hdr_len += take;
        s.hdr[s.hdr_len] = '\0';

        const char *sep = strstr(s.hdr, "\r\n\r\n");
        if (sep == NULL) {
            /* Not yet. If the block filled without a separator the response is
             * malformed (or has absurd headers); give up rather than silently
             * treating header text as body. */
            if (take < n) { s.err = true; s.done = true; }
            return;
        }

        size_t hdr_bytes = (size_t)(sep + 4 - s.hdr);
        size_t spill = s.hdr_len - hdr_bytes;   /* body bytes already in hdr */
        s.hdr_len = hdr_bytes;
        headers_complete();
        if (spill > 0 && !s.done)
            feed_body((const uint8_t *)s.hdr + hdr_bytes, spill);

        /* Bytes that did not fit the header buffer. "The headers completed" and
         * "the buffer filled" are NOT mutually exclusive: a ~1.7 KB header block
         * (routine for a CDN) plus the first body bytes of the same segment
         * overruns HDR_MAX, and the overflow is body, not header. Dropping it
         * silently punches a hole in the stream -- which desynchronises the
         * chunked decoder, or shortens a Content-Length body so completion
         * never fires and the request burns its whole timeout. */
        if (take < n && !s.done) feed_body(p + take, n - take);
        return;
    }
    feed_body(p, n);
}

/* ---- altcp callbacks (run under the lwIP lock) ---- */

static void try_send(struct altcp_pcb *pcb)
{
    while (s.req_sent < s.req_len) {
        u16_t avail = altcp_sndbuf(pcb);
        if (avail == 0) break;
        size_t remain = s.req_len - s.req_sent;
        u16_t n = (remain < avail) ? (u16_t)remain : avail;
        if (altcp_write(pcb, s.req + s.req_sent, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        s.req_sent += n;
    }
    altcp_output(pcb);
}

static err_t sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)len;
    try_send(pcb);
    return ERR_OK;
}

static err_t recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (p == NULL) { s.done = true; return ERR_OK; }   /* peer closed: complete */
    if (err == ERR_OK) {
        for (struct pbuf *q = p; q != NULL && !s.done; q = q->next)
            feed((const uint8_t *)q->payload, q->len);
        altcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
    (void)arg; (void)err;
    s.pcb = NULL;          /* lwIP has freed it */
    s.pcb_dead = true;
    s.err = true;
    s.done = true;
}

static err_t connected_cb(void *arg, struct altcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { s.err = true; s.done = true; return ERR_OK; }
    /* For TLS this fires after the HANDSHAKE completes, not after the TCP
     * connect, so writing the request here is already encrypted. */
    s.req_sent = 0;
    try_send(pcb);
    return ERR_OK;
}

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name; (void)arg;
    if (ipaddr) { s_ip = *ipaddr; s_dns = 1; }
    else        { s_dns = 2; }
}

/* ---- misc parsing ---- */

static int parse_status(void)
{
    if (s.hdr_len < 12 || strncmp(s.hdr, "HTTP/", 5) != 0) return 0;
    const char *sp = memchr(s.hdr, ' ', s.hdr_len);
    return sp ? atoi(sp + 1) : 0;
}

static void parse_etag(char *out, size_t cap)
{
    char raw[96];
    if (!header_value("ETag", raw, sizeof raw)) { out[0] = '\0'; return; }
    const char *v = raw;
    size_t len = strlen(v);
    if (len >= 2 && v[0] == '"' && v[len - 1] == '"') { v++; len -= 2; }
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

/* Parse an RFC 1123 HTTP Date ("Sun, 06 Nov 1994 08:49:37 GMT") to a Unix
 * epoch. Returns 0 if it does not look like a plausible recent timestamp. This
 * is the firmware's primary clock source: it needs no internet (unlike SNTP)
 * and arrives on every response. */
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

/* ---- the request ---- */

bool http_request(const http_req_t *req, http_resp_t *resp)
{
    memset(resp, 0, sizeof *resp);
    if (req == NULL || req->url == NULL) return false;

    /* Parse scheme://host[:port]/path. */
    bool tls = (strncasecmp(req->url, "https://", 8) == 0);
    const char *p = strstr(req->url, "://");
    p = p ? p + 3 : req->url;
    const char *slash = strchr(p, '/');
    const char *colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
    char host[128];
    size_t hlen = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (hlen >= sizeof host) hlen = sizeof host - 1;
    memcpy(host, p, hlen); host[hlen] = '\0';
    uint16_t port = tls ? 443 : 80;
    if (colon) { int v = atoi(colon + 1); if (v > 0 && v < 65536) port = (uint16_t)v; }
    const char *path = slash ? slash : "/";

    /* Build the request. The Host header omits the port when it is the scheme
     * default: some origins compare it literally, and "example.com:443" is not
     * the string their config knows. */
    size_t blen = req->body ? strlen(req->body) : 0;
    int n;
    if (port == (tls ? 443 : 80))
        n = snprintf(s.req, sizeof s.req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                     req->method, path, host);
    else
        n = snprintf(s.req, sizeof s.req,
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n",
                     req->method, path, host, port);
    for (int i = 0; i < req->nheaders && n > 0 && n < (int)sizeof s.req; i++)
        n += snprintf(s.req + n, sizeof s.req - n, "%s\r\n", req->headers[i]);
    if (req->body && n > 0 && n < (int)sizeof s.req)
        n += snprintf(s.req + n, sizeof s.req - n,
                      "Content-Type: %s\r\nContent-Length: %u\r\n",
                      req->content_type ? req->content_type : "application/json",
                      (unsigned)blen);
    if (n > 0 && n < (int)sizeof s.req)
        n += snprintf(s.req + n, sizeof s.req - n, "\r\n");
    if (n <= 0 || n >= (int)sizeof s.req - (int)blen) {
        printf("http: request too large\n");
        return false;
    }
    if (req->body) { memcpy(s.req + n, req->body, blen); n += (int)blen; }
    s.req_len = (size_t)n;

    /* Reset per-request state. */
    s.req_sent = 0;
    s.hdr_len = 0; s.hdr_done = false; s.hdr[0] = '\0';
    s.sink = req->sink; s.sink_cap = req->sink_cap;
    s.body_len = 0; s.overflow = false;
    s.chunked = false; s.have_clen = false; s.clen = 0;
    s.cstate = CH_SIZE; s.chunk_remain = 0; s.sizelen = 0;
    s.done = false; s.err = false; s.pcb_dead = false;
    s_dns = 0;
    s_progress_next = PROGRESS_STEP;
    s_progress_start_ms = (uint32_t)(time_us_64() / 1000ull);

    uint32_t timeout_ms = req->timeout_ms ? req->timeout_ms : 10000;

    /* Resolve. Ask for both families so a v6-only network is reachable; a
     * dual-stack one answers with whichever the resolver prefers. */
    cyw43_arch_lwip_begin();
    err_t de = dns_gethostbyname_addrtype(host, &s_ip, dns_cb, NULL,
                                          LWIP_DNS_ADDRTYPE_IPV4_IPV6);
    cyw43_arch_lwip_end();
    if (de == ERR_OK) s_dns = 1;
    else if (de != ERR_INPROGRESS) {
        printf("http: dns start failed (%d)\n", de);
        return false;
    }
    uint32_t t = 0;
    while (s_dns == 0 && t < timeout_ms) { sleep_ms(20); t += 20; }
    if (s_dns != 1) { printf("http: dns failed for %s\n", host); return false; }

    /* Connect. For TLS the pcb is wrapped before connecting, and the hostname
     * must be attached before the handshake starts -- SNI travels in the very
     * first flight. */
    cyw43_arch_lwip_begin();
    if (tls) {
        struct altcp_tls_config *conf = net_tls_config();
        s.pcb = conf ? altcp_tls_new(conf, IP_GET_TYPE(&s_ip)) : NULL;
        if (s.pcb != NULL && !net_tls_attach(s.pcb, host)) {
            altcp_abort(s.pcb);
            s.pcb = NULL;
            s.pcb_dead = true;
        }
    } else {
        s.pcb = altcp_new_ip_type(NULL, IP_GET_TYPE(&s_ip));
    }
    if (s.pcb != NULL) {
        altcp_arg(s.pcb, NULL);
        altcp_err(s.pcb, err_cb);
        altcp_recv(s.pcb, recv_cb);
        altcp_sent(s.pcb, sent_cb);
        if (altcp_connect(s.pcb, &s_ip, port, connected_cb) != ERR_OK) {
            altcp_abort(s.pcb);
            s.pcb = NULL;
            s.pcb_dead = true;
        }
    }
    cyw43_arch_lwip_end();
    if (s.pcb == NULL) {
        printf("http: connect failed (%s)\n", tls ? "tls setup" : "tcp");
        return false;
    }

    printf("http: %s %s://%s:%u%s\n", req->method, tls ? "https" : "http",
           host, port, path);
    while (!s.done && t < timeout_ms) { sleep_ms(20); t += 20; }

    /* Tear down, unless lwIP already freed it via err_cb. */
    cyw43_arch_lwip_begin();
    if (s.pcb != NULL && !s.pcb_dead) {
        altcp_recv(s.pcb, NULL); altcp_sent(s.pcb, NULL); altcp_err(s.pcb, NULL);
        if (altcp_close(s.pcb) != ERR_OK) altcp_abort(s.pcb);
        s.pcb = NULL;
    }
    cyw43_arch_lwip_end();

    if (!s.done) {
        /* Say WHERE it stalled. "Timeout" alone cannot distinguish a request
         * that was never sent from one whose response never started from one
         * that died part-way through a body -- and those have nothing in
         * common. Chasing the wrong one costs a flash cycle each time. */
        printf("http: timeout after %ums (req %u/%u sent, hdr %u B%s, body %u B%s)\n",
               (unsigned)t, (unsigned)s.req_sent, (unsigned)s.req_len,
               (unsigned)s.hdr_len, s.hdr_done ? " complete" : " INCOMPLETE",
               (unsigned)s.body_len,
               s.chunked ? ", chunked" : (s.have_clen ? ", content-length" : ", no framing"));
        if (s.hdr_done && s.have_clen)
            printf("http:   expected %u body bytes\n", (unsigned)s.clen);
        if (!s.hdr_done && s.hdr_len)
            printf("http:   partial status line: %.60s\n", s.hdr);
        return false;
    }
    if (s.err)    { printf("http: transport error\n"); return false; }
    if (!s.hdr_done) { printf("http: no response headers\n"); return false; }

    resp->status = parse_status();
    parse_etag(resp->etag, sizeof resp->etag);
    resp->body_len  = s.body_len;
    resp->truncated = s.overflow;

    char v[48];
    if (header_value("Retry-After", v, sizeof v)) resp->retry_after_s = atoi(v);
    if (header_value("Date", v, sizeof v)) {
        resp->date_epoch = parse_http_date(v);
        /* Set the wall clock from every response that carries a usable Date.
         * It lands before the status heartbeat needs it, works on a LAN with no
         * internet, and is what makes certificate dates checkable (net_tls.c). */
        if (resp->date_epoch) sleep_set_epoch(resp->date_epoch);
    }

    if (s.sink == NULL) {
        s_inline[s.body_len] = '\0';
        resp->body = s_inline;
    }
    if (s.overflow)
        printf("http: body truncated at %u bytes\n", (unsigned)s.body_len);
    printf("http: <- %d (%u body bytes%s)\n", resp->status,
           (unsigned)s.body_len, s.chunked ? ", chunked" : "");
    return true;
}

bool http_get(const char *url, uint8_t *dst, size_t cap, size_t *out_len,
              uint32_t timeout_ms)
{
    if (out_len) *out_len = 0;

    http_req_t req = {
        .method = "GET",
        .url = url,
        .timeout_ms = timeout_ms,
        .sink = dst,
        .sink_cap = cap,
    };
    http_resp_t resp;
    if (!http_request(&req, &resp)) return false;
    if (out_len) *out_len = resp.body_len;
    return resp.status >= 200 && resp.status < 300 && !resp.truncated;
}
