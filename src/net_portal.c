/*
 * net_portal.c: WiFi/MQTT provisioning captive portal. See net_portal.h.
 *
 * Brings up an open AP, a DHCP server (vendored src/vendor/dhcpserver.c) and a
 * wildcard DNS hijack (vendored src/vendor/dnsserver.c), plus a small raw-TCP
 * HTTP server on port 80 that serves a setup form and saves the submission.
 * The form HTML/CSS/JS is the Tesserae ESP32 client's portal markup verbatim,
 * so the two devices look identical during setup.
 */
#include "net_portal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"

#include "dhcpserver.h"
#include "dnsserver.h"
#include "net_mdns.h"
#include "splash.h"
#include "sleepmgr.h"
#include "config.h"

/* ---------- page markup (verbatim from tesserae-device-esp32-bin) ---------- */

static const char k_head[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<title>Tesserae Setup</title>"
"<style>"
":root{--bg:#f1f0ec;--surface:#fff;--fg:#18181b;--muted:#71706c;"
"--accent:#0d8c7e;--accent-hover:#0a6f63;--accent-soft:#e6f3f1;"
"--border:#e6e5e1;--radius:10px}"
"*{box-sizing:border-box}"
"body{margin:0;padding:24px 16px env(safe-area-inset-bottom);"
"font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Inter,system-ui,sans-serif;"
"font-size:15px;background:var(--bg);color:var(--fg);line-height:1.45;"
"-webkit-text-size-adjust:100%;-webkit-font-smoothing:antialiased}"
"main{max-width:480px;margin:0 auto}"
".brand{display:flex;align-items:center;gap:10px;margin:0 0 6px;"
"font-weight:700;font-size:20px;letter-spacing:-0.015em}"
".brand-mark{flex:none;display:block;width:32px;height:32px;"
"filter:drop-shadow(0 1px 3px rgba(13,140,126,.35))}"
".tag{color:var(--muted);font-size:13px;margin:0 0 18px}"
".card{background:var(--surface);border:1px solid var(--border);"
"border-radius:var(--radius);padding:18px 16px;margin-bottom:14px}"
".card h2{margin:0 0 14px;font-size:11px;text-transform:uppercase;"
"letter-spacing:0.08em;color:var(--muted);font-weight:600}"
".field{margin-bottom:14px}"
".field:last-child{margin-bottom:0}"
"label{display:block;font-weight:500;font-size:13px;margin-bottom:6px;color:var(--fg)}"
"input,select{width:100%;padding:10px 12px;border:1px solid var(--border);"
"border-radius:6px;background:var(--surface);font:inherit;font-size:15px;"
"color:var(--fg);-webkit-appearance:none;appearance:none;"
"transition:border-color .12s,box-shadow .12s}"
"input:focus,select:focus{outline:none;border-color:var(--accent);"
"box-shadow:0 0 0 3px rgba(13,140,126,.18)}"
".pw{position:relative}"
".pw input{padding-right:64px}"
".pw button{position:absolute;right:4px;top:50%;transform:translateY(-50%);"
"background:none;border:0;color:var(--accent);font:inherit;font-size:13px;"
"font-weight:600;padding:8px 10px;cursor:pointer;border-radius:4px}"
".pw button:hover{background:var(--accent-soft)}"
".hint{margin-top:6px;font-size:12px;color:var(--muted)}"
".rescan{display:inline-block;margin-top:10px;font-size:13px;font-weight:600;"
"color:var(--accent);text-decoration:none}"
".rescan:hover{text-decoration:underline}"
".hint code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
"background:#fafaf9;padding:1px 5px;border-radius:3px;border:1px solid var(--border);font-size:11px}"
"button.submit{width:100%;padding:12px 16px;border:0;border-radius:8px;"
"background:var(--accent);color:#fff;font:inherit;font-size:15px;"
"font-weight:600;cursor:pointer;margin-top:4px;transition:background .12s}"
"button.submit:hover{background:var(--accent-hover)}"
"button.submit:active{transform:translateY(1px)}"
"</style></head><body><main>"
"<div class=\"brand\">"
"<svg class=\"brand-mark\" viewBox=\"0 0 256 256\" xmlns=\"http://www.w3.org/2000/svg\" aria-hidden=\"true\">"
"<defs><linearGradient id=\"tg\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
"<stop offset=\"0\" stop-color=\"#0d8c7e\"/><stop offset=\"1\" stop-color=\"#0a6f63\"/></linearGradient>"
"<clipPath id=\"ti\"><rect x=\"55\" y=\"55\" width=\"146\" height=\"146\" rx=\"27\" ry=\"27\"/></clipPath></defs>"
"<rect width=\"256\" height=\"256\" rx=\"72\" ry=\"72\" fill=\"url(#tg)\"/>"
"<g clip-path=\"url(#ti)\" fill=\"#fff\" fill-opacity=\".85\">"
"<rect x=\"128\" y=\"55\" width=\"73\" height=\"73\"/><rect x=\"55\" y=\"128\" width=\"73\" height=\"73\"/></g>"
"</svg>"
"<span>Tesserae</span></div>"
"<p class=\"tag\">Device setup</p>";

/* WiFi card; %s x2 = (ssid prefill, scan-picker HTML or "") */
static const char k_form_wifi_fmt[] =
"<form method=\"POST\" action=\"/save\">"
"<section class=\"card\"><h2>WiFi network</h2>"
"<div class=\"field\">"
"<label for=\"ssid\">Network name (SSID) *</label>"
"<input id=\"ssid\" name=\"ssid\" required maxlength=\"32\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"my-home-wifi\">"
"</div>"
"%s"
"<div class=\"field pw\">"
"<label for=\"wifi-pw\">Password</label>"
"<input id=\"wifi-pw\" name=\"pass\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"
"<button type=\"button\" data-toggle=\"wifi-pw\" aria-label=\"Show password\">Show</button>"
"<p class=\"hint\">Leave blank to keep the current password.</p>"
"</div>"
"</section>";

/* MQTT card; %s x3 = (mqtt_uri, device_id, mqtt_user) */
static const char k_form_mqtt_fmt[] =
"<section class=\"card\"><h2>MQTT broker</h2>"
"<div class=\"field\">"
"<label for=\"mqtt_uri\">Broker URI *</label>"
"<input id=\"mqtt_uri\" name=\"mqtt_uri\" required maxlength=\"159\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"mqtt://192.168.1.50:1883\">"
"</div>"
"<div class=\"field\">"
"<label for=\"device_id\">Device id</label>"
"<input id=\"device_id\" name=\"device_id\" maxlength=\"32\" "
"pattern=\"[a-z][a-z0-9_-]{1,31}\" autocomplete=\"off\" "
"value=\"%s\" placeholder=\"pico\">"
"<p class=\"hint\">Topics: <code>tesserae/&lt;id&gt;/frame/bin</code> etc.</p>"
"</div>"
"<div class=\"field\">"
"<label for=\"mqtt_user\">Username (optional)</label>"
"<input id=\"mqtt_user\" name=\"mqtt_user\" maxlength=\"63\" autocomplete=\"off\" value=\"%s\">"
"</div>"
"<div class=\"field pw\">"
"<label for=\"mqtt-pw\">Password (optional)</label>"
"<input id=\"mqtt-pw\" name=\"mqtt_pass\" type=\"password\" maxlength=\"63\" autocomplete=\"off\">"
"<button type=\"button\" data-toggle=\"mqtt-pw\" aria-label=\"Show password\">Show</button>"
"<p class=\"hint\">Leave blank to keep the current password.</p>"
"</div>"
"</section>"
"<button class=\"submit\" type=\"submit\">Save &amp; restart</button>"
"</form>";

static const char k_tail[] =
"<script>"
"document.querySelectorAll('[data-toggle]').forEach(b=>{"
"b.addEventListener('click',()=>{"
"const i=document.getElementById(b.dataset.toggle);"
"const s=i.type==='password';"
"i.type=s?'text':'password';"
"b.textContent=s?'Hide':'Show';"
"});"
"});"
"const pick=document.getElementById('ssid-pick');"
"if(pick){pick.addEventListener('change',e=>{"
"if(e.target.value){document.getElementById('ssid').value=e.target.value;"
"document.getElementById('wifi-pw').focus();}"
"});}"
"</script></main></body></html>";

static const char k_thanks_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Saved</title><style>"
"body{margin:0;padding:40px 16px;background:#f1f0ec;color:#18181b;"
"font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;text-align:center}"
".card{max-width:380px;margin:0 auto;background:#fff;border:1px solid #e6e5e1;"
"border-radius:10px;padding:28px 20px}"
"h1{margin:0 0 8px;font-size:20px;color:#0d8c7e}"
"p{margin:0;color:#71706c;font-size:14px;line-height:1.5}"
"</style></head><body>"
"<div class=\"card\"><h1>Saved</h1>"
"<p>Tesserae will reboot and apply the new settings now.</p></div>"
"</body></html>";

/* Shown while a rescan runs; auto-reloads the form once it completes. */
static const char k_scanning_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<meta http-equiv=\"refresh\" content=\"9;url=/\"><title>Scanning</title><style>"
"body{margin:0;padding:40px 16px;background:#f1f0ec;color:#18181b;"
"font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;text-align:center}"
".card{max-width:380px;margin:0 auto;background:#fff;border:1px solid #e6e5e1;"
"border-radius:10px;padding:28px 20px}"
"h1{margin:0 0 8px;font-size:20px;color:#0d8c7e}"
"p{margin:0;color:#71706c;font-size:14px;line-height:1.5}"
"</style></head><body>"
"<div class=\"card\"><h1>Scanning&hellip;</h1>"
"<p>Looking for nearby networks. This page reloads automatically.</p></div>"
"</body></html>";

/* ---------- helpers (from the esp32 portal) ---------- */

static void html_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (const char *p = src; *p; p++) {
        const char *rep;
        switch (*p) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '"': rep = "&quot;"; break;
            default:
                if (o + 1 >= dst_sz) { dst[o] = '\0'; return; }
                dst[o++] = *p;
                continue;
        }
        size_t rl = strlen(rep);
        if (o + rl >= dst_sz) break;
        memcpy(dst + o, rep, rl);
        o += rl;
    }
    dst[o] = '\0';
}

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = '\0';
}

/* Pull a named field out of an x-www-form-urlencoded body into dst (decoded).
 * Returns true if the key was present. */
static bool form_field(const char *body, const char *key, char *dst, size_t dst_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= dst_sz) n = dst_sz - 1;
            memcpy(dst, v, n);
            dst[n] = '\0';
            url_decode(dst);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* ---------- WiFi scan (populate the form's network picker) ---------- */

#define SCAN_MAX 16
static char s_scan[SCAN_MAX][33];
static int  s_scan_count;

static int scan_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    if (!r || r->ssid_len == 0 || r->ssid_len > 32) return 0;
    if (s_scan_count >= SCAN_MAX) return 0;
    char ssid[33];
    memcpy(ssid, r->ssid, r->ssid_len);
    ssid[r->ssid_len] = '\0';
    for (int i = 0; i < s_scan_count; i++)
        if (strcmp(s_scan[i], ssid) == 0) return 0;   /* dedup */
    memcpy(s_scan[s_scan_count++], ssid, (size_t)r->ssid_len + 1);
    return 0;
}

/* Scan for nearby APs and (re)fill the picker cache. Runs in whatever mode is
 * active (we scan while the AP is up so a rescan never drops the client). Must
 * be called from the main loop, not an lwIP callback (it blocks ~5s). */
static void wifi_scan(void)
{
    s_scan_count = 0;
    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof opts);
    cyw43_arch_lwip_begin();
    int err = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_cb);
    cyw43_arch_lwip_end();
    if (err != 0) { printf("portal: scan start failed (%d)\n", err); return; }
    uint32_t t = 0;
    while (cyw43_wifi_scan_active(&cyw43_state) && t < 8000) { sleep_ms(100); t += 100; }
    printf("portal: scan found %d networks\n", s_scan_count);
}

/* Render the network picker (a <select> of scanned SSIDs when any) plus a
 * Rescan link, into dst. Always emits the field so the user can rescan. */
static void build_picker(char *dst, size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(dst + n, cap - n, "<div class=\"field\">");
    if (s_scan_count > 0) {
        n += (size_t)snprintf(dst + n, cap - n,
            "<label for=\"ssid-pick\">Or pick a nearby network</label>"
            "<select id=\"ssid-pick\"><option value=\"\">Choose a network&hellip;</option>");
        for (int i = 0; i < s_scan_count && n < cap - 96; i++) {
            char esc[100];
            html_escape(s_scan[i], esc, sizeof esc);
            n += (size_t)snprintf(dst + n, cap - n, "<option>%s</option>", esc);
        }
        n += (size_t)snprintf(dst + n, cap - n, "</select>");
    }
    n += (size_t)snprintf(dst + n, cap - n,
        "<a class=\"rescan\" href=\"/rescan\">&#x21bb; %s</a></div>",
        s_scan_count > 0 ? "Rescan networks" : "Scan for networks");
}

/* ---------- HTTP server (raw TCP) ---------- */

#define REQ_MAX   1280
#define PAGE_MAX  8192

static char           s_resp[PAGE_MAX];   /* one connection serviced at a time */
static volatile bool  s_reboot_pending;
static volatile bool  s_rescan_pending;   /* set by GET /rescan; run in main loop */
static volatile bool  s_client_seen;      /* set on each connection; resets idle timer */

typedef struct {
    char     req[REQ_MAX];
    size_t   req_len;
    size_t   resp_total;
    size_t   acked;
    bool     reboot_after;
} conn_t;

static void conn_close(struct tcp_pcb *pcb, conn_t *c)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    if (c) free(c);
    tcp_close(pcb);
}

/* Build the setup form into s_resp (HTTP response incl. headers). */
static size_t build_form(void)
{
    const config_t *cfg = config_get();
    char ssid[80], uri[200], devid[80], user[80];
    html_escape(cfg->wifi_ssid, ssid, sizeof ssid);
    html_escape(cfg->mqtt_uri, uri, sizeof uri);
    html_escape(cfg->mqtt_device_id, devid, sizeof devid);
    html_escape(cfg->mqtt_user, user, sizeof user);

    /* Body is assembled after the header; we backfill Content-Length once known.
     * Simpler: build body in a scratch, then emit headers + body. */
    static char picker[640];
    build_picker(picker, sizeof picker);

    static char body[PAGE_MAX - 256];
    int n = 0;
    n += snprintf(body + n, sizeof body - n, "%s", k_head);
    n += snprintf(body + n, sizeof body - n, k_form_wifi_fmt, ssid, picker);
    n += snprintf(body + n, sizeof body - n, k_form_mqtt_fmt, uri, devid, user);
    n += snprintf(body + n, sizeof body - n, "%s", k_tail);

    int h = snprintf(s_resp, sizeof s_resp,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    if (h + n >= (int)sizeof s_resp) n = (int)sizeof s_resp - h - 1;
    memcpy(s_resp + h, body, n);
    return (size_t)(h + n);
}

static size_t build_static(const char *html)
{
    int n = (int)strlen(html);
    int h = snprintf(s_resp, sizeof s_resp,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    memcpy(s_resp + h, html, n);
    return (size_t)(h + n);
}

/* Apply a submitted form body to config and persist. */
static void save_form(const char *body)
{
    char ssid[40] = {0}, pass[80] = {0};
    char uri[200] = {0}, devid[40] = {0}, user[72] = {0}, mpass[72] = {0};
    bool has_ssid  = form_field(body, "ssid", ssid, sizeof ssid);
    bool has_pass  = form_field(body, "pass", pass, sizeof pass);
    bool has_uri   = form_field(body, "mqtt_uri", uri, sizeof uri);
    bool has_devid = form_field(body, "device_id", devid, sizeof devid);
    bool has_user  = form_field(body, "mqtt_user", user, sizeof user);
    bool has_mpass = form_field(body, "mqtt_pass", mpass, sizeof mpass);

    /* NULL means "leave unchanged"; a blank password keeps the existing one. */
    config_set_wifi(has_ssid ? ssid : NULL,
                    (has_pass && pass[0]) ? pass : NULL);
    config_set_mqtt(has_uri ? uri : NULL,
                    has_devid ? devid : NULL,
                    has_user ? user : NULL,
                    (has_mpass && mpass[0]) ? mpass : NULL);
    printf("portal: saving config (ssid='%s' uri='%s' devid='%s')\n", ssid, uri, devid);
    printf(config_save() ? "portal: config saved\n" : "portal: SAVE FAILED\n");
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    conn_t *c = (conn_t *)arg;
    if (!c) return ERR_OK;
    c->acked += len;
    if (c->acked >= c->resp_total) {
        bool reboot = c->reboot_after;
        conn_close(pcb, c);
        if (reboot) s_reboot_pending = true;
    }
    return ERR_OK;
}

static void send_response(struct tcp_pcb *pcb, conn_t *c, size_t total)
{
    c->resp_total = total;
    c->acked = 0;
    tcp_sent(pcb, sent_cb);
    tcp_write(pcb, s_resp, (u16_t)total, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static void handle_request(struct tcp_pcb *pcb, conn_t *c)
{
    bool is_post = strncmp(c->req, "POST", 4) == 0;
    const char *body = strstr(c->req, "\r\n\r\n");
    body = body ? body + 4 : "";

    if (is_post && strstr(c->req, "/save")) {
        save_form(body);
        c->reboot_after = true;
        send_response(pcb, c, build_static(k_thanks_html));
    } else if (strstr(c->req, "GET /rescan")) {
        s_rescan_pending = true;
        send_response(pcb, c, build_static(k_scanning_html));
    } else {
        send_response(pcb, c, build_form());
    }
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = (conn_t *)arg;
    if (p == NULL) { conn_close(pcb, c); return ERR_OK; }   /* peer closed */
    if (err != ERR_OK || c == NULL) { pbuf_free(p); return ERR_OK; }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        size_t n = q->len;
        if (c->req_len + n > REQ_MAX - 1) n = REQ_MAX - 1 - c->req_len;
        memcpy(c->req + c->req_len, q->payload, n);
        c->req_len += n;
    }
    c->req[c->req_len] = '\0';
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    /* Wait for the end of headers; for POST, also for the full body. */
    const char *hdr_end = strstr(c->req, "\r\n\r\n");
    if (!hdr_end) {
        if (c->req_len >= REQ_MAX - 1) handle_request(pcb, c);   /* oversized; respond anyway */
        return ERR_OK;
    }
    if (strncmp(c->req, "POST", 4) == 0) {
        const char *cl = strstr(c->req, "Content-Length:");
        size_t want = cl ? (size_t)atoi(cl + 15) : 0;
        size_t have = c->req_len - (size_t)((hdr_end + 4) - c->req);
        if (have < want && c->req_len < REQ_MAX - 1) return ERR_OK;   /* need more body */
    }
    handle_request(pcb, c);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
    (void)err;
    if (arg) free(arg);   /* pcb already freed by lwIP */
}

static err_t accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || pcb == NULL) return ERR_VAL;
    conn_t *c = (conn_t *)calloc(1, sizeof(conn_t));
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    s_client_seen = true;   /* activity: keep the portal awake */
    tcp_arg(pcb, c);
    tcp_recv(pcb, recv_cb);
    tcp_err(pcb, err_cb);
    return ERR_OK;
}

/* ---------- portal entry ---------- */

void portal_run(const panel_t *panel, uint8_t variant)
{
    /* Per-board suffix from the flash unique ID (always available; the CYW43
     * interface MACs read 0 before the interface is up). */
    pico_unique_board_id_t bid;
    pico_get_unique_board_id(&bid);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "Tesserae-Setup-%02X%02X",
             bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2],
             bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);

    /* Paint the setup splash first (instructions + join QR) while the radio is
     * still down, then bring up the AP. */
    splash_show_setup(panel, variant, ssid);

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE)) {
        printf("portal: cyw43 init failed\n");
        return;
    }
    printf("portal: starting AP '%s' (open); browse to http://192.168.4.1/\n", ssid);

    cyw43_arch_enable_ap_mode(ssid, NULL, CYW43_AUTH_OPEN);

    ip_addr_t gw, mask;
    IP4_ADDR(ip_2_ip4(&gw), 192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&mask), 255, 255, 255, 0);

    static dhcp_server_t dhcp;
    static dns_server_t  dns;
    dhcp_server_init(&dhcp, &gw, &mask);
    dns_server_init(&dns, &gw);
    mdns_advertise();   /* also reachable at tesserae-pico-XXXX.local on the AP */

    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb && tcp_bind(pcb, IP_ANY_TYPE, 80) == ERR_OK) {
        pcb = tcp_listen_with_backlog(pcb, 2);
        tcp_accept(pcb, accept_cb);
        printf("portal: HTTP server listening on :80\n");
    } else {
        printf("portal: failed to bind :80\n");
    }
    cyw43_arch_lwip_end();

    /* Initial scan (while the AP is up, so the client never drops) to pre-fill
     * the picker. Best-effort: the form still works by typing the SSID. */
    wifi_scan();

    /* Service the portal (threadsafe-background runs lwIP for us) until the user
     * submits the form. A Rescan link sets s_rescan_pending, which we service
     * here in the main loop (never in an lwIP callback, since the scan blocks).
     *
     * Idle timeout: if no client connects for IDLE_TIMEOUT_MS, power the AP down
     * with no wake source so a forgotten setup AP does not drain the battery.
     * A reset (button) brings it back into the portal. Any connection resets
     * the timer. */
    const uint32_t IDLE_TIMEOUT_MS = 15u * 60u * 1000u;
    uint32_t idle_ms = 0;
    while (!s_reboot_pending) {
        if (s_rescan_pending) {
            s_rescan_pending = false;
            printf("portal: rescan requested\n");
            wifi_scan();
        }
        if (s_client_seen) { s_client_seen = false; idle_ms = 0; }
        else if ((idle_ms += 50) >= IDLE_TIMEOUT_MS) {
            printf("portal: no activity for 15 min; powering down (press reset to wake)\n");
            sleep_deep_until_reset();   /* does not return */
        }
        sleep_ms(50);
    }
    printf("portal: settings saved, rebooting\n");
    sleep_ms(800);   /* let the "Saved" response flush to the browser */
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}
