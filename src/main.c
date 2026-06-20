/*
 * tesserae-device-pico-bin: Tesserae e-paper client firmware (ESP32-client parity).
 *
 * One cycle per boot: detect the attached Pimoroni Inky panel from its model
 * EEPROM, connect WiFi, sync time over SNTP, fetch the retained frame URL over
 * MQTT, download the image into SRAM/PSRAM, paint it (skipping if unchanged),
 * publish status, then deep-sleep until the next refresh. POWMAN deep sleep
 * resets the chip, so each wake re-runs main() from the top. With sleep_s <= 0
 * the device stays awake and loops the cycle (dev mode).
 *
 * Only the 13.3" (EL133UF1) is verified on hardware; the other panel drivers
 * are blind ports from the Pimoroni reference and are flagged UNTESTED.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/sha256.h"

#include "inky_eeprom.h"
#include "panels.h"
#include "net_wifi.h"
#include "net_mqtt.h"
#include "net_rest.h"
#include "net_http.h"
#include "net_sntp.h"
#include "net_portal.h"
#include "net_mdns.h"
#include "config.h"
#include "psram.h"
#include "sleepmgr.h"

#define FW_VERSION  "0.2.0"

/* Consecutive cycles that fail to reach WiFi/the server before the firmware
 * re-opens the setup portal (so a wrong WiFi password or server URL can be fixed
 * without a reflash). At ~1 cycle/minute this is a few minutes of retries. */
#ifndef MAX_CONNECT_FAILS
#define MAX_CONNECT_FAILS  3
#endif

#define DEV_LOOP_INTERVAL_MS  60000   /* re-check for a new frame this often in dev mode */

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/* 64-hex-char SHA-256 of a string, via the RP2350 hardware SHA block. */
static void sha256_hex(const char *s, char out[65])
{
    pico_sha256_state_t st;
    if (pico_sha256_try_start(&st, SHA256_BIG_ENDIAN, false) != 0) { out[0] = '\0'; return; }
    pico_sha256_update(&st, (const uint8_t *)s, strlen(s));
    sha256_result_t r;
    pico_sha256_finish(&st, &r);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", r.bytes[i]);
    out[64] = '\0';
}

/*
 * One Tesserae cycle: WiFi up, sync time, fetch the retained frame URL, dedup
 * by SHA-256, download into SRAM (small panels) or PSRAM (the 13.3"), publish
 * status, drop the radio, then paint. Painting happens with the radio down so
 * the slow refresh and the post-paint flash write are safe.
 */
static bool do_cycle(const panel_t *panel, uint8_t variant, size_t psram_sz)
{
    const config_t *c = config_get();
    const uint8_t *frame      = NULL;   /* what we'll paint; NULL -> stripe test pattern */
    uint8_t       *sram_buf   = NULL;   /* malloc'd buffer to free after paint, if any   */
    bool           skip_paint = false;
    bool           cfg_dirty  = false;
    char           new_hash[65] = {0};

    bool wifi_ok = config_has_wifi() && wifi_connect(c->wifi_ssid, c->wifi_pass, 20000);
    if (wifi_ok) {
        mdns_advertise();  /* discoverable as tesserae-pico-XXXX.local while awake */
        sntp_sync(8000);   /* best-effort; never blocks the cycle */

        if (c->mqtt_uri[0] != '\0') {
            char url[200];
            int32_t sleep_s = c->sleep_s;
            bool got_url = mqtt_fetch_retained(c->mqtt_uri, c->mqtt_device_id,
                                               c->mqtt_user, c->mqtt_pass,
                                               url, sizeof url, &sleep_s, 8000);
#ifdef DEV_FRAME_URL
            /* Dev override: bypass the retained topic and fetch a fixed URL. */
            snprintf(url, sizeof url, "%s", DEV_FRAME_URL);
            got_url = true;
            printf("dev: forced frame url %s\n", url);
#endif
#ifdef DEV_SLEEP_S
            sleep_s = DEV_SLEEP_S;   /* dev override: wins over broker/flash */
#endif
            if (sleep_s != c->sleep_s) { config_set_sleep_s(sleep_s); cfg_dirty = true; }

            if (got_url && panel != NULL) {
                sha256_hex(url, new_hash);
                bool dedup_skip = (new_hash[0] && strcmp(new_hash, c->last_hash) == 0);
#ifdef DEV_FORCE_REPAINT
                dedup_skip = false;   /* dev override: always re-download and repaint */
#endif
                if (dedup_skip) {
                    printf("frame: unchanged (hash match); skipping refresh\n");
                    skip_paint = true;
                } else {
                    uint32_t need = panel_frame_bytes(panel);
                    uint8_t *buf = NULL;
                    if (need <= 256u * 1024) buf = sram_buf = malloc(need);
                    if (buf == NULL && psram_sz >= need) buf = (uint8_t *)PSRAM_XIP_BASE;
                    if (buf == NULL) {
                        printf("frame: no buffer for %u bytes (PSRAM board needed?)\n",
                               (unsigned)need);
                    } else {
                        size_t got = 0;
                        if (http_get(url, buf, need, &got, 30000) && got == need) {
                            frame = buf;
                        } else {
                            printf("frame: fetch failed or wrong size (got %u, want %u)\n",
                                   (unsigned)got, (unsigned)need);
                        }
                    }
                }
            } else if (!got_url) {
                printf("mqtt: no retained frame url\n");
            }
        } else {
            printf("mqtt: no broker configured (set MQTT_URI in secrets.h)\n");
        }

        /* Heartbeat (full ESP32-client parity). Battery is reported as 0 (this
         * board has no battery-sense ADC wired). wake_reason uses the server's
         * vocabulary ("timer"/"poweron"); sleep_until is omitted when the clock
         * is unsynced so the server falls back to its tolerance window. */
        char ip[16];
        wifi_get_ip(ip, sizeof ip);
        int32_t      interval = c->sleep_s;
        uint32_t     epoch    = sleep_epoch_now();
        const char  *wreason  = (sleep_wake_reason() == WAKE_TIMER) ? "timer" : "poweron";
        char status[320];
        int n = snprintf(status, sizeof status,
                 "{\"fw_version\":\"%s\",\"kind\":\"pico_bin_client\","
                 "\"battery_mv\":0,\"battery_pct\":0,"
                 "\"rssi\":%d,\"ip\":\"%s\",\"panel_w\":%u,\"panel_h\":%u,"
                 "\"sleep_interval_s\":%ld,\"next_sleep_s\":%ld,"
                 "\"wake_reason\":\"%s\",\"boot\":%u",
                 FW_VERSION,
                 wifi_rssi(), ip, panel ? panel->width : 0, panel ? panel->height : 0,
                 (long)interval, (long)interval, wreason, (unsigned)sleep_boot_count());
        if (n > 0 && (size_t)n < sizeof status) {
            if (epoch && interval > 0)
                snprintf(status + n, sizeof status - n,
                         ",\"sleep_until\":%lu}", (unsigned long)(epoch + interval));
            else
                snprintf(status + n, sizeof status - n, "}");
        }
        if (c->mqtt_uri[0] != '\0')
            mqtt_publish_status(c->mqtt_uri, c->mqtt_device_id, c->mqtt_user,
                                c->mqtt_pass, status, 5000);
        wifi_stop();   /* radio down before the slow refresh + flash write */
    } else if (!config_has_wifi()) {
        printf("wifi: no SSID configured; skipping (portal comes in a later phase)\n");
    }

    /* Decide what to paint:
     *   - dedup hit  -> skip (frame unchanged).
     *   - got a frame -> paint it, remember its hash.
     *   - no frame this cycle (no URL / fetch failed): if we have painted a real
     *     frame before (last_hash set), KEEP the current image rather than
     *     clobbering it with the stripe test pattern. The server briefly clears
     *     the retained frame during a re-render, so a transient miss is normal.
     *     Only paint stripes on a fresh device that has never shown a frame, as
     *     a liveness/bring-up indicator. */
    if (skip_paint) {
        printf("paint: skipped (unchanged)\n");
    } else if (panel == NULL) {
        /* no driver; nothing to paint */
    } else if (frame != NULL) {
        printf("painting downloaded frame (this takes ~20-45s)...\n");
        panel->paint(variant, frame);
        if (new_hash[0]) { config_set_last_hash(new_hash); cfg_dirty = true; }
        printf("done.\n");
    } else if (c->last_hash[0] != '\0') {
        printf("paint: no frame this cycle; keeping last image\n");
    } else {
        printf("painting test stripes (no frame yet; this takes ~20-45s)...\n");
        panel->paint(variant, NULL);
        printf("done.\n");
    }
    if (cfg_dirty) {   /* radio is down here, so the flash write is safe */
        printf(config_save() ? "config: saved\n" : "config: SAVE FAILED\n");
    }
    if (sram_buf != NULL) free(sram_buf);
    return wifi_ok;   /* connectivity reached this cycle (for the portal fallback) */
}

/* Resolve a (possibly relative) frame URL against the server origin. */
static void resolve_url(const char *server, const char *u, char *out, size_t cap)
{
    if (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0) {
        snprintf(out, cap, "%s", u);
        return;
    }
    char origin[200];
    snprintf(origin, sizeof origin, "%s", server);
    char *p = strstr(origin, "://");
    p = p ? p + 3 : origin;
    char *sl = strchr(p, '/');
    if (sl) *sl = '\0';                      /* drop any path on the server_url */
    snprintf(out, cap, "%s%s%s", origin, (u[0] == '/') ? "" : "/", u);
}

/*
 * First-boot bootstrap for the REST transport. Returns true once a device token
 * is in hand (continue the cycle); false means "no token yet" and a backoff has
 * been written to config.sleep_s for the caller to sleep on. Default is the
 * zero-touch discover/claim flow (admin clicks Register in Tesserae, no typing
 * on the device); a stored pairing code opts into strict admin-gated register.
 */
static bool rest_bootstrap(uint16_t pw, uint16_t ph, const char *mac,
                           bool *dirty, bool *reached)
{
    const config_t *c = config_get();
    if (c->device_token[0] != '\0') return true;   /* already bootstrapped */

    if (c->pairing_code[0] != '\0') {
        rest_register_out_t ro;
        rest_status_t rs = rest_register(pw, ph, mac, FW_VERSION, &ro, 10000);
        *reached = (rs != REST_NET_ERR);
        if (rs == REST_OK) {
            config_set_device_token(ro.token);
            /* Adopt the server's canonical device id (it matches us by MAC, so it
             * may differ from our configured id); the token is bound to it, and
             * frame/status URLs must use it or the server returns 403. */
            if (ro.device_id[0] && strcmp(ro.device_id, config_device_id()) != 0)
                config_set_device_id(ro.device_id);
            config_set_pairing_code("");            /* one-shot; clear on success */
            if (ro.sleep_interval_s > 0) config_set_sleep_s(ro.sleep_interval_s);
            if (ro.server_time) sleep_set_epoch(ro.server_time);
            *dirty = true;
            printf("rest: registered via pairing code; token stored (id=%s)\n",
                   config_device_id());
            return true;
        }
        int32_t backoff = (c->sleep_s > 0) ? c->sleep_s : 900;
        if (rs == REST_UNAUTH || rs == REST_FORBIDDEN) {
            backoff = 3600;
            printf("rest: register rejected (%d); sleeping 1h to re-pair\n", rs);
        } else if (rs == REST_RATELIMIT) {
            backoff = (ro.retry_after_s > 0) ? ro.retry_after_s : 3600;
            printf("rest: register rate-limited; backoff %lds\n", (long)backoff);
        } else {
            printf("rest: register failed (%d); retry next cycle\n", rs);
        }
        config_set_sleep_s(backoff);
        return false;
    }

    /* Zero-touch: announce via discover and claim the token once the admin
     * clicks Register. Retried every wake, by design (no caching). */
    rest_discover_out_t dd;
    rest_status_t ds = rest_discover(pw, ph, mac, FW_VERSION, &dd, 10000);
    /* "reached" only on a real discover response (200/429). A wrong URL that
     * happens to answer with a 4xx/5xx still counts as a failure so the portal
     * fallback fires, instead of looping on a bad endpoint forever. */
    *reached = (ds == REST_OK || ds == REST_RATELIMIT);
    if (ds == REST_OK && dd.registered) {
        config_set_device_token(dd.token);
        /* Adopt the server's canonical device id (MAC-matched; may differ from
         * our configured id). The token is bound to it -- frame/status URLs must
         * use it or the server returns 403 "token not valid for this device". */
        if (dd.device_id[0] && strcmp(dd.device_id, config_device_id()) != 0) {
            printf("rest: adopting server device id '%s' (was '%s')\n",
                   dd.device_id, config_device_id());
            config_set_device_id(dd.device_id);
        }
        if (dd.sleep_interval_s > 0) config_set_sleep_s(dd.sleep_interval_s);
        if (dd.server_time) sleep_set_epoch(dd.server_time);
        *dirty = true;
        printf("rest: claimed token via discover; bootstrap complete (id=%s)\n",
               config_device_id());
        return true;
    }
    int32_t backoff;
    if (ds == REST_OK) {            /* registered:false, awaiting admin Register */
        backoff = (dd.retry_after_s > 0) ? dd.retry_after_s : 30;
        printf("rest: discovered, waiting for admin to Register; retry in %lds\n", (long)backoff);
    } else if (ds == REST_RATELIMIT) {
        backoff = (dd.retry_after_s > 0) ? dd.retry_after_s : 60;
        printf("rest: discover rate-limited; backoff %lds\n", (long)backoff);
    } else {
        backoff = 15;   /* unreachable (e.g. wrong server URL): retry soon so the
                         * connectivity-failure portal fallback kicks in quickly */
        printf("rest: discover failed (%d); retry in %lds\n", ds, (long)backoff);
    }
    config_set_sleep_s(backoff);
    return false;
}

/*
 * One Tesserae cycle over the REST API (used when a server_url is configured).
 * Bootstrap a token if needed, GET the frame (ETag/304 dedup), download + paint,
 * POST status, and let next_poll_s drive the deep sleep. Mirrors do_cycle's
 * "paint with the radio down" ordering. Sleep duration is left in config.sleep_s
 * for main() to act on; error paths set a backoff there.
 */
static bool do_cycle_rest(const panel_t *panel, uint8_t variant, size_t psram_sz)
{
    const config_t *c = config_get();
    uint8_t       *sram_buf  = NULL;
    const uint8_t *frame     = NULL;
    bool           skip_paint = false;
    bool           cfg_dirty  = false;
    bool           reached    = false;   /* did we reach the server this cycle? */
    char           new_etag[80] = {0};

    if (!config_has_wifi() || !wifi_connect(c->wifi_ssid, c->wifi_pass, 20000)) {
        printf("rest: wifi unavailable; skipping cycle\n");
        return false;   /* main() counts this toward the portal fallback */
    }
    mdns_advertise();
    /* No SNTP on the REST path: the wall clock is set from each response's HTTP
     * Date header (works on a LAN with no internet, and saves ~8s/cycle). */

    char mac[18];
    wifi_get_mac(mac, sizeof mac);

    /* Report the panel in its native landscape orientation (the pico_bin
     * renderer packs 1600x1200 for the 13.3"). */
    uint16_t pw = panel ? panel->width : 0, ph = panel ? panel->height : 0;
    if (pw < ph) { uint16_t t = pw; pw = ph; ph = t; }

    /* 1. Bootstrap a device token if we have none (discover/claim by default,
     * register-with-code when one is stored). Stop and sleep until we have one. */
    if (c->device_token[0] == '\0') {
        bool boot_dirty = false;
        bool got = rest_bootstrap(pw, ph, mac, &boot_dirty, &reached);
        if (!got) {
            /* No token yet. The backoff lives in config.sleep_s (RAM) and is
             * consumed by this boot's sleep, so there is nothing to persist on
             * the common "waiting for admin" retry: avoid the flash wear. */
            wifi_stop();
            return reached;   /* reached==false (server unreachable) feeds the fallback */
        }
        if (boot_dirty) cfg_dirty = true;
        reached = true;
        c = config_get();
    }

    /* 2. Frame metadata, with If-None-Match for the ETag/304 dedup. */
    rest_frame_out_t fo;
    rest_status_t fs = rest_get_frame(&fo, 10000);
    if (fs != REST_NET_ERR) reached = true;
    if (fs == REST_OK) {
        snprintf(new_etag, sizeof new_etag, "%s", fo.etag);
        if (panel != NULL) {
            char fullurl[512];
            resolve_url(c->server_url, fo.url, fullurl, sizeof fullurl);
            uint32_t need = panel_frame_bytes(panel);
            uint8_t *buf = NULL;
            if (need <= 256u * 1024) buf = sram_buf = malloc(need);
            if (buf == NULL && psram_sz >= need) buf = (uint8_t *)PSRAM_XIP_BASE;
            if (buf == NULL) {
                printf("rest: no buffer for %u bytes (PSRAM board needed?)\n", (unsigned)need);
            } else {
                size_t got = 0;
                if (http_get(fullurl, buf, need, &got, 30000) && got == need) {
                    frame = buf;
                } else {
                    printf("rest: frame fetch failed or wrong size (got %u, want %u)\n",
                           (unsigned)got, (unsigned)need);
                }
            }
        }
    } else if (fs == REST_NOT_MODIFIED) {
        printf("rest: frame unchanged (304); skipping paint\n");
        skip_paint = true;
    } else if (fs == REST_NO_CONTENT) {
        printf("rest: no frame rendered yet (204); skipping paint\n");
        skip_paint = true;
    } else if (fs == REST_UNAUTH || fs == REST_FORBIDDEN) {
        printf("rest: frame auth failed (%d); wiping token to re-register next cycle\n", fs);
        config_set_device_token("");
        cfg_dirty = true;
    } else {
        printf("rest: frame request failed (%d); keeping last image\n", fs);
    }

    /* 3. Status heartbeat (only while we still hold a token). */
    if (config_get()->device_token[0] != '\0') {
        char ip[16];
        wifi_get_ip(ip, sizeof ip);
        int32_t  interval    = config_get()->sleep_s;
        uint32_t epoch       = sleep_epoch_now();
        uint32_t sleep_until = (epoch && interval > 0) ? (epoch + interval) : 0;
        rest_status_out_t so;
        rest_status_t ss = rest_post_status(wifi_rssi(), ip, pw, ph,
                                            interval, sleep_until, FW_VERSION, &so, 8000);
        if (ss != REST_NET_ERR) reached = true;
        if (ss == REST_OK) {
            if (so.server_time) sleep_set_epoch(so.server_time);
            if (so.sleep_interval_s > 0 && so.sleep_interval_s != config_get()->sleep_s) {
                config_set_sleep_s(so.sleep_interval_s);
                cfg_dirty = true;
            }
            if (so.next_poll_s > 0) config_set_sleep_s(so.next_poll_s);   /* drives this sleep */
        } else {
            printf("rest: status post failed (%d)\n", ss);
            if (ss == REST_UNAUTH || ss == REST_FORBIDDEN) {
                config_set_device_token(""); cfg_dirty = true;
            }
        }
    }

    wifi_stop();   /* radio down before the slow refresh + flash write */

#ifdef DEV_SLEEP_S
    config_set_sleep_s(DEV_SLEEP_S);   /* dev override: wins over server next_poll_s */
#endif

    /* Paint decision (radio down). Keep the current image on a transient miss if
     * we have ever painted a frame (last_frame_etag set). */
    if (skip_paint) {
        printf("paint: skipped (unchanged / none)\n");
    } else if (panel == NULL) {
        /* no driver */
    } else if (frame != NULL) {
        printf("painting downloaded frame (this takes ~20-45s)...\n");
        panel->paint(variant, frame);
        if (new_etag[0]) { config_set_frame_etag(new_etag); cfg_dirty = true; }
        printf("done.\n");
    } else if (config_get()->last_frame_etag[0] != '\0') {
        printf("paint: no frame this cycle; keeping last image\n");
    } else {
        printf("painting test stripes (no frame yet; this takes ~20-45s)...\n");
        panel->paint(variant, NULL);
        printf("done.\n");
    }

    if (cfg_dirty) {
        printf(config_save() ? "config: saved\n" : "config: SAVE FAILED\n");
    }
    if (sram_buf != NULL) free(sram_buf);
    return reached;
}

/* Run one cycle on the active transport, tracking connectivity. After
 * MAX_CONNECT_FAILS consecutive failures to reach WiFi/the server, re-open the
 * setup portal so a bad WiFi password or server URL can be corrected without a
 * reflash. The failure counter lives in always-on scratch (survives the
 * deep-sleep reboots between cycles). */
static void run_cycle(const panel_t *panel, uint8_t variant, size_t psram_sz)
{
    bool reached = config_has_server() ? do_cycle_rest(panel, variant, psram_sz)
                                       : do_cycle(panel, variant, psram_sz);
    if (reached) { sleep_failcount_reset(); return; }

    sleep_failcount_inc();
    uint32_t fails = sleep_failcount();
    printf("net: connectivity failure %u/%u\n", (unsigned)fails, MAX_CONNECT_FAILS);
    if (fails >= MAX_CONNECT_FAILS) {
        printf("net: too many failures; re-opening setup portal\n");
        sleep_failcount_reset();
        wifi_stop();                 /* release cyw43 so the portal can re-init it */
        portal_run(panel, variant);  /* does not return (reboots on save) */
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);   /* let a freshly attached USB serial monitor catch the logs */
    printf("\ntesserae-device-pico-bin\n");

    /* Phase 4a (WIP): bring up PSRAM (needed only for frames too big for SRAM,
     * e.g. the 13.3"). Returns 0 on boards without it. The self-test is a
     * temporary de-risk check and will be trimmed once the fetch path uses it. */
    size_t psram_sz = psram_init(PSRAM_CS_PIN_PLUS2);
    if (psram_sz) {
        printf("psram: %u bytes detected; self-test %s\n",
               (unsigned)psram_sz, psram_test(psram_sz) ? "PASS" : "FAIL");
    } else {
        printf("psram: none detected (large panels need a PSRAM board)\n");
    }

    /* Phase 2 (WIP, building toward ESP32-client parity): load persistent
     * config from flash. On first boot (blank flash) seed it from secrets.h if
     * present, then save. Radio is down here, so the flash write is safe. */
    bool had_config = config_load();
    config_t before = *config_get();
    /* secrets.h acts as a development override: any values it defines are
     * applied (and re-saved if they changed) on every boot. Once the
     * provisioning portal exists, stored config will take precedence and
     * secrets.h becomes a first-boot seed only. */
#ifdef WIFI_SSID
    config_set_wifi(WIFI_SSID, WIFI_PASS);
#endif
#ifdef MQTT_URI
    config_set_mqtt(MQTT_URI,
#  ifdef MQTT_DEVICE_ID
                    MQTT_DEVICE_ID,
#  else
                    NULL,
#  endif
#  ifdef MQTT_USER
                    MQTT_USER, MQTT_PASS
#  else
                    NULL, NULL
#  endif
    );
#endif
    bool changed = !had_config ||
                   memcmp(&before, config_get(), sizeof(config_t)) != 0;
    if (changed) {
        printf(config_save() ? "config: saved (from secrets.h / defaults)\n"
                             : "config: SAVE FAILED\n");
    } else {
        printf("config: loaded from flash\n");
    }
    const config_t *c = config_get();
    printf("config: wifi_ssid='%s' device_id='%s' sleep_s=%ld last_hash=%s\n",
           c->wifi_ssid, c->mqtt_device_id[0] ? c->mqtt_device_id : "(default)",
           (long)c->sleep_s, c->last_hash[0] ? "set" : "(none)");

    /* Identify the panel from the model EEPROM (I2C, separate from the SPI panel
     * bus). The display_variant selects the driver. */
    inky_i2c_init();
    inky_eeprom_t ee;
    uint8_t variant = 0xFF;
    if (inky_eeprom_read(&ee)) {
        variant = ee.display_variant;
        printf("eeprom: %ux%u variant=%u (%s)\n",
               ee.width, ee.height, ee.display_variant,
               inky_display_variant_name(ee.display_variant));
    } else {
        printf("eeprom: no response at 0x50; cannot identify panel\n");
    }
    const panel_t *panel = panel_for_variant(variant);
    if (panel != NULL) {
        printf("panel: %s (%ux%u)%s\n", panel->name, panel->width, panel->height,
               panel->verified ? "" : "  [UNTESTED driver]");
    } else {
        printf("no driver for variant %u\n", variant);
    }

    /* Phase 5: report why we woke (cold boot vs POWMAN timer wake) and start
     * the always-on wall-clock timer (it survives deep sleep once SNTP sets it). */
    sleep_timer_init();
    sleep_calibrate_lposc();   /* one-time LPOSC trim (cold boot only) for accurate sleep */
    wake_reason_t wake = sleep_wake_reason();
    printf("wake: %s (boot #%u)\n",
           wake == WAKE_TIMER ? "timer" : "cold", (unsigned)sleep_boot_count());
    if (wake == WAKE_COLD) sleep_failcount_reset();   /* fresh power-on: clear backoff */

#ifdef DEV_FORCE_PORTAL
    printf("dev: forcing provisioning portal\n");
    portal_run(panel, variant);
#endif

    /* No WiFi credentials yet: come up as a setup AP and serve the provisioning
     * portal. Submitting the form saves config and reboots, so this never
     * returns. (secrets.h, if present, seeds creds and skips this.) */
    if (!config_has_wifi()) {
        printf("config: no WiFi credentials; entering provisioning portal\n");
        portal_run(panel, variant);
    }

    /* Run one cycle, then either deep-sleep until the next refresh or, in dev
     * mode (sleep_s <= 0), stay awake and loop so the serial monitor survives
     * and the device can be reflashed. sleep_s may be updated by the cycle.
     * Transport: REST when a server_url is configured, else the MQTT path.
     * run_cycle re-opens the portal after repeated connectivity failures. */
    run_cycle(panel, variant, psram_sz);

    int32_t sleep_s = config_get()->sleep_s;
    if (sleep_s > 0) {
        uint32_t now = sleep_epoch_now();
        if (now) printf("sleep: until epoch %lu\n", (unsigned long)(now + sleep_s));
        printf("sleep: deep sleep for %ld s (wake reboots)\n", (long)sleep_s);
        sleep_deep_ms((uint64_t)sleep_s * 1000ull);   /* does not return */
    }

    printf("dev: sleep_s<=0, staying awake; re-checking every %ds\n",
           DEV_LOOP_INTERVAL_MS / 1000);
    while (true) {
        sleep_ms(DEV_LOOP_INTERVAL_MS);
        run_cycle(panel, variant, psram_sz);
    }
}
