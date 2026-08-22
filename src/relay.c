/* relay.c -- Tesserae cloud-relay transport. See relay.h. */

#include "relay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_httpc.h"
#include "relay_crypto.h"
#include "relay_wire.h"
#include "config.h"
#include "battery.h"
#include "sleepmgr.h"
#include "net_tls.h"

#define RELAY_HTTP_MS   15000   /* TLS handshake + round trip, over the internet */
#define RELAY_FRAME_MS  60000   /* a ~960 KB sealed frame */

/* ETag of a frame that decrypted but has not reached the glass yet. Held here
 * until relay_commit_frame(), so a frame that fails to paint is re-fetched next
 * wake instead of being skipped by a 304 forever. */
static char s_pending_etag[80];

/* config_etag advertised by THIS wake's status response, if it carried one.
 * Lets relay_sync_config() skip its request when the advertised value matches
 * what we already hold. Deliberately RAM-only and per-wake: a stale hint from a
 * previous boot could suppress a fetch that is genuinely due. */
static char s_advertised_cfg_etag[80];
static bool s_have_advertised_cfg;

/* One revocation vote per wake. A wake makes up to THREE device-token requests
 * (frame, status, config); without this, a single revoked wake would score 3
 * and unpair immediately, collapsing "two consecutive wakes" into "one wake"
 * and handing a transient 401 the power to unpair on the spot. RAM, so it
 * resets each boot -- which is exactly the wake boundary being counted. */
static bool s_auth_noted_this_wake;

/* Fold one device-token response into the revocation streak. 401 counts; any
 * answer the relay could only give a VALID token clears it. Anything else
 * (timeout, 5xx, DNS) is left alone -- it says nothing about the token. */
static void note_auth(int status)
{
    if (status == 401) {
        if (s_auth_noted_this_wake) return;
        s_auth_noted_this_wake = true;
        sleep_relay_authfail_inc();
        printf("relay: answered 401 (streak %u); pairing may be revoked\n",
               (unsigned)sleep_relay_authfail());
    } else if (status == 304 || (status >= 200 && status < 300)) {
        /* The relay only serves these to a live token, so the pairing is good
         * whatever an earlier request in this wake looked like. */
        sleep_relay_authfail_reset();
        s_auth_noted_this_wake = false;
    }
}

bool relay_pairing_revoked(void)
{
    return sleep_relay_authfail() >= 2;
}

void relay_forget_revoked_pairing(void)
{
    printf("relay: pairing revoked; clearing mailbox identity and key\n");
    config_clear_relay();
    sleep_relay_authfail_reset();
    /* The caller persists this once the radio is down: config_save() erases a
     * flash sector with interrupts off, which is not safe while WiFi is up. */
}

/* ---- small JSON HTTP helper ---------------------------------------------
 *
 * Pairing, status and the config document are tiny JSON round trips, so they
 * use net_httpc's internal response buffer. Frames stream into a caller buffer
 * instead (relay_fetch_frame). Returns the HTTP status, or -1 on a transport
 * failure; *body_out points into net_httpc's static buffer. */
static int relay_json(const char *url, const char *method, const char *body,
                      const char *bearer, const char **body_out,
                      size_t *body_len_out, char *etag_out, size_t etag_cap)
{
    char auth[300];
    const char *hdrs[1];
    int nh = 0;
    if (bearer && bearer[0]) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
        hdrs[nh++] = auth;
    }

    http_req_t req = {
        .method = method,
        .url = url,
        .headers = hdrs,
        .nheaders = nh,
        .body = body,
        .timeout_ms = RELAY_HTTP_MS,
    };
    http_resp_t resp;
    bool ok = http_request(&req, &resp);
#ifdef TESSERAE_TLS_DEBUG
    net_tls_log_stack("after-request");
#endif
    if (!ok) {
        printf("relay: %s %s failed (transport)\n", method, url);
        return -1;
    }
    if (body_out)     *body_out = resp.body;
    if (body_len_out) *body_len_out = resp.body_len;
    if (etag_out && etag_cap) snprintf(etag_out, etag_cap, "%s", resp.etag);
    return resp.status;
}

/* ---- state --------------------------------------------------------------- */

bool relay_ready(void)
{
    return config_relay_ready();
}

bool relay_pairing_pending(void)
{
    const config_t *c = config_get();
    return c->relay_url[0] && c->relay_code[0] && !config_relay_ready();
}

/* ---- pairing ------------------------------------------------------------- */

/* Submit our public key for the operator's code. 404 means the code is unknown
 * or expired -- a terminal answer, not something to retry into a lockout. */
static relay_pair_result_t pair_submit(const config_t *c,
                                       uint16_t panel_w, uint16_t panel_h,
                                       bool *dirty)
{
    uint8_t priv[RELAY_PRIV_LEN], pub[RELAY_PUB_LEN];
    relay_keypair(priv, pub);

    char pub_b64[RELAY_B64_KEY_CAP];
    if (!relay_b64url_encode(pub_b64, sizeof pub_b64, pub, sizeof pub))
        return RELAY_PAIR_ERROR;

    /* Self-report the hardware facts the panel already knows, so the operator
     * does not have to retype them when adding a remote panel. Wrong answers
     * here are silent: a panel told the wrong kind gets a mis-packed frame. */
    char body[512];
    if (!relay_build_pair_body(body, sizeof body, c->relay_code, pub_b64,
                               (int)panel_w, (int)panel_h,
                               RELAY_DEVICE_MODEL, RELAY_DEVICE_GAMUT)) {
        printf("relay: could not build the pair request body\n");
        return RELAY_PAIR_ERROR;
    }

    char url[256];
    snprintf(url, sizeof url, "%s/v1/pair", c->relay_url);
    int st = relay_json(url, "POST", body, NULL, NULL, NULL, NULL, 0);

    if (st == 404) {
        printf("relay: pairing code rejected (expired or unknown); clearing\n");
        config_clear_relay();
        *dirty = true;
        return RELAY_PAIR_EXPIRED;
    }
    if (st < 200 || st >= 300) {
        printf("relay: POST /v1/pair -> %d\n", st);
        return RELAY_PAIR_ERROR;
    }

    /* Persist the private key BEFORE reporting progress: pairing polls can span
     * deep sleeps, and losing the scalar would strand the code (which is
     * single-use for the POST, so it could not simply be retried). The same
     * write stamps the start of the pairing window. */
    config_set_relay_priv(priv, sleep_raw_ms());
    *dirty = true;
    printf("relay: pairing submitted (%ux%u, %s/%s); waiting for the home instance\n",
           (unsigned)panel_w, (unsigned)panel_h,
           RELAY_DEVICE_MODEL, RELAY_DEVICE_GAMUT);
    return RELAY_PAIR_WAITING;
}

/* Poll for completion and, on "ready", derive + persist the frame key.
 *
 * The poll route has NO expiry check, so this may span as many deep-sleep wakes
 * as home's completion poller needs. The 10-minute code expiry is enforced only
 * by POST /v1/pair, which is why pair_submit() runs exactly once. */
/* Has this pairing attempt outlived its window?
 *
 * The elapsed check has to survive the wall clock being set, because that moves
 * the very timer this measures (sleep_raw_ms). A backwards jump, or a delta
 * larger than the window could plausibly produce, is a clock correction rather
 * than elapsed time -- so re-baseline and keep going. Erring toward "not
 * expired" is deliberate: wrongly expiring costs the operator a physical trip
 * to type a fresh code, while wrongly continuing costs one more poll. */
static bool pair_window_expired(const config_t *c, bool *dirty)
{
    if (c->relay_pair_started_ms == 0) return false;   /* not started yet */

    uint64_t now = sleep_raw_ms();
    if (now < c->relay_pair_started_ms ||
        (now - c->relay_pair_started_ms) > RELAY_PAIR_WINDOW_MS * 2) {
        printf("relay: pairing clock jumped; restarting the %u h window\n",
               (unsigned)(RELAY_PAIR_WINDOW_MS / 3600000ull));
        config_set_relay_pair_started(now);
        *dirty = true;
        return false;
    }

    uint64_t elapsed = now - c->relay_pair_started_ms;
    if (elapsed <= RELAY_PAIR_WINDOW_MS) {
        printf("relay: pairing pending for %u min of %u h\n",
               (unsigned)(elapsed / 60000ull),
               (unsigned)(RELAY_PAIR_WINDOW_MS / 3600000ull));
        return false;
    }
    return true;
}

static relay_pair_result_t pair_poll(const config_t *c, bool *dirty)
{
    if (pair_window_expired(c, dirty)) {
        printf("relay: pairing window elapsed without completion; clearing\n");
        config_clear_relay();
        *dirty = true;
        return RELAY_PAIR_EXPIRED;
    }

    char url[256];
    snprintf(url, sizeof url, "%s/v1/pair/%s", c->relay_url, c->relay_code);

    const char *resp = NULL;
    size_t resp_len = 0;
    int st = relay_json(url, "GET", NULL, NULL, &resp, &resp_len, NULL, 0);

    if (st == 404) {
        printf("relay: pairing code expired before completion; clearing\n");
        config_clear_relay();
        *dirty = true;
        return RELAY_PAIR_EXPIRED;
    }
    if (st < 200 || st >= 300) return RELAY_PAIR_ERROR;

    relay_pairing_t pr;
    switch (relay_parse_ready(resp, resp_len, &pr)) {
    case RELAY_READY_PENDING:
        return RELAY_PAIR_WAITING;          /* home has not completed it yet */
    case RELAY_READY_MALFORMED:
        printf("relay: pairing response malformed or missing a field\n");
        return RELAY_PAIR_ERROR;
    default:
        break;
    }

    if (!c->relay_have_priv) {
        /* Our scalar is gone (config wiped mid-pair). The code is single-use for
         * the POST, so there is no way back: start over. */
        printf("relay: no panel private key held; re-pair required\n");
        config_clear_relay();
        *dirty = true;
        return RELAY_PAIR_EXPIRED;
    }

    uint8_t key[RELAY_KEY_LEN];
    if (!relay_derive_key(key, c->relay_priv, pr.home_pub)) {
        printf("relay: frame key derivation failed (degenerate peer key)\n");
        return RELAY_PAIR_ERROR;
    }

    config_set_relay_paired(pr.install_id, pr.device_id, pr.device_token, key);
    config_set_relay_etag("");              /* new mailbox: nothing to match */
    config_set_relay_config_etag("");
    memset(key, 0, sizeof key);
    *dirty = true;
    printf("relay: paired (install=%s device=%s)\n", pr.install_id, pr.device_id);
    return RELAY_PAIR_DONE;
}

relay_pair_result_t relay_pair_step(uint16_t panel_w, uint16_t panel_h,
                                    bool *dirty)
{
    bool local_dirty = false;
    if (dirty == NULL) dirty = &local_dirty;

    const config_t *c = config_get();
    if (!c->relay_url[0] || !c->relay_code[0]) return RELAY_PAIR_IDLE;
    if (config_relay_ready()) return RELAY_PAIR_IDLE;
    return c->relay_have_priv ? pair_poll(c, dirty)
                              : pair_submit(c, panel_w, panel_h, dirty);
}

/* ---- frames -------------------------------------------------------------- */

relay_frame_result_t relay_fetch_frame(uint8_t *buf, size_t cap,
                                       const uint8_t **frame, size_t *len,
                                       bool *dirty)
{
    bool local_dirty = false;
    if (dirty == NULL) dirty = &local_dirty;
    if (frame) *frame = NULL;
    if (len) *len = 0;
    if (!config_relay_ready() || buf == NULL) return RELAY_FRAME_ERROR;

    const config_t *c = config_get();
    char url[320];
    if (!relay_mailbox_url(url, sizeof url, c->relay_url, c->relay_install,
                           c->relay_device, "frame")) {
        printf("relay: cannot build the frame URL from stored ids\n");
        return RELAY_FRAME_ERROR;
    }

    char auth[300], inm[128];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->relay_token);
    const char *hdrs[2] = { auth, NULL };
    int nh = 1;
    if (c->relay_etag[0]) {
        snprintf(inm, sizeof inm, "If-None-Match: \"%s\"", c->relay_etag);
        hdrs[nh++] = inm;
    }

    http_req_t req = {
        .method = "GET",
        .url = url,
        .headers = hdrs,
        .nheaders = nh,
        .timeout_ms = RELAY_FRAME_MS,
        .sink = buf,
        .sink_cap = cap,
    };
    http_resp_t resp;
    if (!http_request(&req, &resp)) return RELAY_FRAME_ERROR;
    note_auth(resp.status);

    if (resp.status == 304) return RELAY_FRAME_UNCHANGED;
    if (resp.status == 204) return RELAY_FRAME_NONE;
    if (resp.status < 200 || resp.status >= 300) {
        printf("relay: frame GET -> %d\n", resp.status);
        return RELAY_FRAME_ERROR;
    }
    if (resp.truncated || resp.body_len == 0) {
        printf("relay: sealed frame is %u bytes (buffer holds %u); dropping\n",
               (unsigned)resp.body_len, (unsigned)cap);
        return RELAY_FRAME_ERROR;
    }

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (!relay_unseal(buf, resp.body_len, c->relay_key, &plain, &plain_len)) {
        /* A failed GCM tag is the one error that must never be painted: the
         * mailbox does not match our key, so the bytes are someone else's (or
         * corrupt). Drop the cached ETag so the next poll refetches rather than
         * 304-ing onto the same bad blob forever. */
        printf("relay: sealed frame FAILED AUTHENTICATION (%u bytes); dropping\n",
               (unsigned)resp.body_len);
        config_set_relay_etag("");
        *dirty = true;
        return RELAY_FRAME_ERROR;
    }

    /* Stash the ETag; only committed once the frame actually paints. */
    snprintf(s_pending_etag, sizeof s_pending_etag, "%s", resp.etag);

    if (frame) *frame = plain;
    if (len) *len = plain_len;
    printf("relay: frame %u bytes sealed -> %u plaintext (etag %s)\n",
           (unsigned)resp.body_len, (unsigned)plain_len,
           resp.etag[0] ? resp.etag : "(none)");
    return RELAY_FRAME_NEW;
}

void relay_commit_frame(bool *dirty)
{
    if (!s_pending_etag[0]) return;
    config_set_relay_etag(s_pending_etag);
    if (dirty) *dirty = true;
    s_pending_etag[0] = '\0';
}

/* ---- status -------------------------------------------------------------- */

bool relay_post_status(int rssi, const char *ip, uint16_t panel_w,
                       uint16_t panel_h, const char *fw_version)
{
    if (!config_relay_ready()) return false;
    const config_t *c = config_get();

    int mv = (int)battery_mv();
    char body[512];
    if (!relay_build_status_body(body, sizeof body, c->relay_device, fw_version,
                                 panel_w, panel_h, ip, rssi,
                                 mv, mv > 0 ? battery_pct() : 0, c->tz)) {
        printf("relay: could not build the status body\n");
        return false;
    }

    char url[320];
    if (!relay_mailbox_url(url, sizeof url, c->relay_url, c->relay_install,
                           c->relay_device, "status"))
        return false;

    const char *resp = NULL;
    size_t resp_len = 0;
    int st = relay_json(url, "POST", body, c->relay_token, &resp, &resp_len,
                        NULL, 0);
    note_auth(st);

    if (st < 200 || st >= 300) {
        printf("relay: status post -> %d\n", st);
        return false;
    }

    /* The response carries the current config etag when a config document
     * exists, so posting status doubles as the change notification and
     * relay_sync_config() can skip its request entirely when nothing moved.
     * Absent ({} before anything was published) simply leaves no hint. */
    s_advertised_cfg_etag[0] = '\0';
    s_have_advertised_cfg = relay_parse_config_etag(resp, resp_len,
                                                    s_advertised_cfg_etag,
                                                    sizeof s_advertised_cfg_etag);
    return true;
}

/* ---- device config ------------------------------------------------------- */

relay_config_result_t relay_sync_config(bool *dirty)
{
    bool local_dirty = false;
    if (dirty == NULL) dirty = &local_dirty;
    if (!config_relay_ready()) return RELAY_CFG_ERROR;
    const config_t *c = config_get();

    /* Cheapest path: this wake's status response advertised exactly the etag we
     * already hold, so the document cannot have changed and we skip the request
     * altogether. When status was not posted, or advertised nothing, fall
     * through to a conditional GET -- If-None-Match makes that nearly free and
     * it is what keeps config current on wakes with no status. */
    if (s_have_advertised_cfg && c->relay_config_etag[0] &&
        strcmp(s_advertised_cfg_etag, c->relay_config_etag) == 0)
        return RELAY_CFG_UNCHANGED;

    char url[320];
    if (!relay_mailbox_url(url, sizeof url, c->relay_url, c->relay_install,
                           c->relay_device, "config")) {
        printf("relay: cannot build the config URL from stored ids\n");
        return RELAY_CFG_ERROR;
    }

    char auth[300], inm[128];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->relay_token);
    const char *hdrs[2] = { auth, NULL };
    int nh = 1;
    if (c->relay_config_etag[0]) {
        snprintf(inm, sizeof inm, "If-None-Match: \"%s\"", c->relay_config_etag);
        hdrs[nh++] = inm;
    }

    /* Sealed exactly like a frame, just far smaller: nonce + ciphertext + tag,
     * decrypted in place in this scratch buffer. */
    static uint8_t doc[1024];
    http_req_t req = {
        .method = "GET",
        .url = url,
        .headers = hdrs,
        .nheaders = nh,
        .timeout_ms = RELAY_HTTP_MS,
        .sink = doc,
        .sink_cap = sizeof doc,
    };
    http_resp_t resp;
    if (!http_request(&req, &resp)) return RELAY_CFG_ERROR;
    note_auth(resp.status);

    if (resp.status == 304) return RELAY_CFG_UNCHANGED;
    if (resp.status == 204) return RELAY_CFG_NONE;   /* none published yet */
    if (resp.status < 200 || resp.status >= 300) {
        printf("relay: config GET -> %d\n", resp.status);
        return RELAY_CFG_ERROR;      /* non-fatal: keep the last-known config */
    }
    if (resp.truncated || resp.body_len == 0) return RELAY_CFG_ERROR;

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (!relay_unseal(doc, resp.body_len, c->relay_key, &plain, &plain_len)) {
        /* Same rule as a frame: a failed tag means the mailbox does not match
         * our key, so the contents are not ours to act on. Drop the cached etag
         * so the next wake refetches instead of 304-ing onto the same bad blob. */
        printf("relay: sealed config FAILED AUTHENTICATION (%u bytes); dropping\n",
               (unsigned)resp.body_len);
        config_set_relay_config_etag("");
        *dirty = true;
        return RELAY_CFG_ERROR;
    }

    relay_devcfg_t cfg;
    if (!relay_parse_config((const char *)plain, plain_len, &cfg)) {
        printf("relay: config document is not a JSON object; ignoring\n");
        return RELAY_CFG_ERROR;
    }

    /* Absent fields mean "keep what we have", so each is guarded by its own
     * sentinel rather than written unconditionally. */
    bool changed = false;
    if (cfg.sleep_interval_s > 0 && cfg.sleep_interval_s != config_get()->sleep_s) {
        config_set_sleep_s(cfg.sleep_interval_s);
        changed = true;
    }
    if (cfg.tz[0] && strcmp(cfg.tz, config_get()->tz) != 0) {
        config_set_tz(cfg.tz);
        changed = true;
    }
    if (cfg.always_on >= 0 && config_set_always_on(cfg.always_on != 0)) {
        printf("relay: always_on -> %s\n", cfg.always_on ? "on" : "off");
        changed = true;
    }

    /* Store the etag only now: everything above succeeded, so a 304 next wake
     * genuinely means "you already have this". Always persisted, even when no
     * value moved, so an unchanged document is not re-fetched every wake. */
    config_set_relay_config_etag(resp.etag);
    *dirty = true;

    printf("relay: config applied (etag %s): sleep=%lds tz=%s always_on=%d%s\n",
           resp.etag[0] ? resp.etag : "(none)",
           (long)config_get()->sleep_s,
           config_get()->tz[0] ? config_get()->tz : "(none)",
           config_get()->always_on ? 1 : 0,
           changed ? "" : " (no change)");
    return RELAY_CFG_APPLIED;
}
