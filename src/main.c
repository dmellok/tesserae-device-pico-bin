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
#include "hardware/watchdog.h"

#include "inky_eeprom.h"
#include "panels.h"
#include "net_wifi.h"
#include "net_mqtt.h"
#include "net_rest.h"
#include "net_httpc.h"
#include "net_sntp.h"
#include "net_portal.h"
#include "net_mdns.h"
#include "config.h"
#include "psram.h"
#include "sleepmgr.h"
#include "battery.h"
#include "buttons.h"
#include "splash.h"
#include "fw_version.h"
#include "relay.h"
#include "relay_crypto.h"
#include "net_tls.h"

/* Consecutive cycles that fail to reach WiFi/the server before the firmware
 * re-opens the setup portal (so a wrong WiFi password or server URL can be fixed
 * without a reflash). At ~1 cycle/minute this is a few minutes of retries. */
#ifndef MAX_CONNECT_FAILS
#define MAX_CONNECT_FAILS  3
#endif

/* Low-battery hibernation thresholds, in VSYS millivolts (see battery.h: on
 * this board VSYS is the cell, and reads ~4.7 V whenever USB is attached).
 *
 * A single LiPo cell is essentially empty at 3.3 V, and driving a full e-paper
 * refresh below that browns out mid-waveform, which leaves the glass in a
 * half-written state that looks like a hardware fault. Stopping at 3.25 V keeps
 * enough margin for the one goodbye refresh. The resume threshold is well above
 * it so a cell hovering at the line does not oscillate between hibernating and
 * waking, which would drain it faster than simply staying down. */
#define BATTERY_GOODBYE_MV   3250u
#define BATTERY_RESUME_MV    3450u

/* How long to hibernate on a flat cell before re-checking.
 *
 * tesserae-device-firmware powers down with NO wake source here, on the
 * reasoning that a flat cell should draw nothing at all. This board diverges
 * deliberately: POWMAN's power-down cannot be interrupted by plugging in USB
 * (the pad domain is off), so an indefinite hibernate means a charged device
 * stays dark until someone finds the reset button. An hourly wake costs a few
 * tens of milliseconds of ADC read, which is nothing against a dead battery,
 * and makes "plug it in and walk away" work. */
#define BATTERY_RECHECK_S    3600

/* How long BTN_A must be held during the boot window to wipe the config.
 * Long enough that it cannot happen by accident (the device is usually being
 * handled when it is reset), short enough to be a believable instruction. */
#define FACTORY_RESET_HOLD_MS  10000

/* Whether status splashes may paint this boot. See maybe_show_status(). */
static bool s_status_splash_allowed;

/* Whether a status splash has already been painted this boot. One diagnosis per
 * wake: the cycle functions know the specific cause (wrong password, server
 * down, waiting on approval) and paint it, and run_cycle's generic fallback
 * must not then paint over it with a vaguer -- and wrong -- message. */
static bool s_status_splash_painted;

/* Optional startup delay so a serial monitor attached right after reset/wake can
 * catch the boot logs. It is wasted awake time on every (battery) wake, so the
 * default is 0; set DEV_BOOT_LOG_DELAY_MS (e.g. 2000) in secrets.h for on-bench
 * debugging. */
#ifndef DEV_BOOT_LOG_DELAY_MS
#define DEV_BOOT_LOG_DELAY_MS  0
#endif

#define DEV_LOOP_INTERVAL_MS  60000   /* re-check for a new frame this often in dev mode */

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/* ---------- onboarding feedback ---------- */

/* Paint a status screen, if it is worth the refresh.
 *
 * Two gates, and both matter:
 *
 *   1. State change. What is on the glass lives in an always-on scratch word,
 *      so a failure that repeats every wake repaints nothing. Without this a
 *      device with a wrong WiFi password would burn a 20-45 s refresh a minute,
 *      forever.
 *
 *   2. Only when the panel is not already earning its keep. Once a real frame
 *      has been painted, a single failed poll must NOT replace the user's
 *      dashboard with an error card -- transient failures are normal and the
 *      stale dashboard is far more useful than "can't reach the server". After
 *      a cold boot (which includes the reboot out of the setup portal) there is
 *      nothing to protect, so the status screens are exactly what is wanted.
 *
 *   3. At most one per boot. Two different diagnoses in one wake would mean two
 *      full refreshes back to back -- around 90 s -- and the user would be left
 *      looking at whichever painted last, which is the LESS specific one.
 *
 * The result: setup narrates itself, and steady-state operation stays quiet. */
static void maybe_show_status(const panel_t *panel, uint8_t variant,
                              splash_state_t st)
{
    if (panel == NULL || !s_status_splash_allowed || s_status_splash_painted) return;
    if ((splash_state_t)sleep_splash_state() == st) {
        s_status_splash_painted = true;   /* already correct on the glass */
        return;
    }
    splash_show_status(panel, variant, st);
    sleep_set_splash_state((uint32_t)st);
    s_status_splash_painted = true;
}

/* Record that a real frame is on the glass, so the next failure is visible. */
static void note_frame_painted(void)
{
    sleep_set_splash_state((uint32_t)SPLASH_NONE);
}

/* ---------- low-battery goodbye ---------- */

/* Decide whether the cell can support another cycle, and act on it.
 *
 * On a flat cell: paint the goodbye once, then hibernate; a wake that finds it
 * still flat hibernates again without repainting. On recovery, the goodbye is
 * still on the glass and the server's frame has NOT changed -- so the next poll
 * would 304 and leave "Battery empty" up indefinitely. Clearing the cached ETag
 * and hash here is what forces exactly one real repaint.
 *
 * Does not return when the battery is flat. */
static void battery_goodbye_check(const panel_t *panel, uint8_t variant)
{
    uint32_t mv = battery_mv();
    if (mv == 0) return;   /* never sampled / no sense: do not act on a non-reading */

    bool showing = (sleep_splash_state() == (uint32_t)SPLASH_BATTERY_EMPTY);
    uint32_t threshold = showing ? BATTERY_RESUME_MV : BATTERY_GOODBYE_MV;
    if (mv >= threshold) {
        if (showing) {
            printf("battery: recovered (%lu mV); forcing one repaint\n",
                   (unsigned long)mv);
            /* Every transport's conditional-fetch key has to go, not just the
             * REST one: whichever is in use would otherwise answer 304 (the
             * server's frame has not changed while we were asleep) and leave
             * "Battery empty" on the glass indefinitely. */
            config_set_frame_etag("");
            config_set_last_hash("");
            config_set_relay_etag("");
            sleep_set_splash_state((uint32_t)SPLASH_NONE);
            config_save();     /* radio is still down here, so this is safe */
        }
        return;
    }

    if (!showing) {
        printf("battery: empty (%lu mV); painting goodbye and hibernating\n",
               (unsigned long)mv);
        splash_show_status(panel, variant, SPLASH_BATTERY_EMPTY);
        sleep_set_splash_state((uint32_t)SPLASH_BATTERY_EMPTY);
    } else {
        printf("battery: still empty (%lu mV); hibernating\n", (unsigned long)mv);
    }
    sleep_deep_ms((uint64_t)BATTERY_RECHECK_S * 1000ull);   /* does not return */
}

/* ---------- factory reset ---------- */

static void factory_reset_tick(unsigned elapsed)
{
    printf("buttons: A held %us/%us -- release to cancel the factory reset\n",
           elapsed / 1000u, (unsigned)FACTORY_RESET_HOLD_MS / 1000u);
}

/* Wipe the stored config if BTN_A is held through the boot window.
 *
 * This is the only escape hatch from a device that cannot reach its portal --
 * a saved-but-wrong server URL on a network whose SSID still connects, say,
 * where the connectivity-failure fallback never fires. It runs BEFORE anything
 * touches WiFi or flash, so it works even when the rest of the cycle would not.
 *
 * Buttons cannot wake this board from deep sleep (see buttons.h), so the
 * gesture is: hold A, tap RESET, keep holding. Does not return if it fires. */
static void maybe_factory_reset(void)
{
    if (!button_down(BTN_A)) return;

    printf("buttons: A is down -- hold %us for a factory reset (release to cancel)\n",
           (unsigned)FACTORY_RESET_HOLD_MS / 1000u);
    if (!button_held_for(BTN_A, FACTORY_RESET_HOLD_MS, factory_reset_tick)) {
        printf("buttons: released; factory reset cancelled\n");
        return;
    }

    printf("buttons: FACTORY RESET -- erasing config, rebooting into setup\n");
    printf(config_erase() ? "config: erased\n" : "config: ERASE FAILED\n");
    sleep_set_splash_state((uint32_t)SPLASH_NONE);
    sleep_force_cold_boot();
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

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

    bool wifi_dirty = false;
    bool wifi_ok = config_has_wifi() &&
                   wifi_connect(c->wifi_ssid, c->wifi_pass, 20000, &wifi_dirty);
    if (wifi_dirty) cfg_dirty = true;
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

        /* Heartbeat (full ESP32-client parity). Battery is sampled from VSYS at
         * cycle start (see battery_sample in run_cycle). wake_reason uses the
         * server's vocabulary ("timer"/"poweron"); sleep_until is omitted when
         * the clock is unsynced so the server falls back to its tolerance window. */
        char ip[16];
        wifi_get_ip(ip, sizeof ip);
        int32_t      interval = c->sleep_s;
        uint32_t     epoch    = sleep_epoch_now();
        const char  *wreason  = (sleep_wake_reason() == WAKE_TIMER) ? "timer" : "poweron";
        char status[320];
        int n = snprintf(status, sizeof status,
                 "{\"fw_version\":\"%s\",\"kind\":\"pico_bin_client\","
                 "\"battery_mv\":%lu,\"battery_pct\":%d,"
                 "\"rssi\":%d,\"ip\":\"%s\",\"panel_w\":%u,\"panel_h\":%u,"
                 "\"sleep_interval_s\":%ld,\"next_sleep_s\":%ld,"
                 "\"wake_reason\":\"%s\",\"boot\":%u",
                 FW_VERSION,
                 (unsigned long)battery_mv(), battery_pct(),
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
    } else {
        /* Failed to associate. wifi_connect leaves the CYW43 driver up on
         * failure, and the paint + config_save below both need it down. */
        wifi_stop();
        maybe_show_status(panel, variant, SPLASH_WIFI_FAILED);
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
        note_frame_painted();
        printf("done.\n");
    } else if (c->last_hash[0] != '\0') {
        printf("paint: no frame this cycle; keeping last image\n");
    } else if (s_status_splash_painted) {
        /* A status screen already went on the glass this wake (the Wi-Fi
         * failure above, most likely). Painting stripes over it would cost a
         * second 20-45 s refresh AND destroy the diagnosis -- and because the
         * recorded splash state would still say WIFI_FAILED, no later wake
         * would ever repaint the message. Permanent stripes, no explanation. */
        printf("paint: status screen is up; skipping the stripe fallback\n");
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

    bool wifi_dirty = false;
    if (!config_has_wifi() ||
        !wifi_connect(c->wifi_ssid, c->wifi_pass, 20000, &wifi_dirty)) {
        printf("rest: wifi unavailable; skipping cycle\n");
        /* wifi_connect leaves the CYW43 driver INITIALISED on failure -- only
         * wifi_stop() tears it down -- so the radio is still live here. Both a
         * flash erase (interrupts off for ~100 ms, XIP disconnected) and a
         * 20-45 s panel refresh have to wait until it is not. */
        wifi_stop();
        if (wifi_dirty) config_save();
        /* Say so on the glass: a wrong password is the single most common setup
         * failure, and it is invisible without a serial cable. */
        maybe_show_status(panel, variant, SPLASH_WIFI_FAILED);
        return false;   /* main() counts this toward the portal fallback */
    }
    if (wifi_dirty) cfg_dirty = true;
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
            /* "Waiting for the admin to click Register" is NOT a failure, and
             * saying so is the difference between a user who waits and a user
             * who starts re-flashing. A server we could not reach at all is a
             * different message with a different fix. */
            maybe_show_status(panel, variant,
                              reached ? SPLASH_AWAITING_APPROVAL
                                      : SPLASH_SERVER_UNREACHABLE);
            if (cfg_dirty) config_save();
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
            /* Absent tz means "keep what we have", not "clear it": an older
             * server simply never sends the key. */
            if (so.tz[0] && strcmp(so.tz, config_get()->tz) != 0) {
                printf("rest: adopting timezone '%s'\n", so.tz);
                config_set_tz(so.tz);
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

    net_tls_deinit();   /* frees the parsed root store if this server was https */
    wifi_stop();        /* radio down before the slow refresh + flash write */

#ifdef DEV_SLEEP_S
    config_set_sleep_s(DEV_SLEEP_S);   /* dev override: wins over server next_poll_s */
#endif

    /* Paint decision (radio down). Keep the current image on a transient miss if
     * we have ever painted a frame (last_frame_etag set). */
    if (skip_paint && fs == REST_NO_CONTENT && config_get()->last_frame_etag[0] == '\0') {
        /* Onboarded, but the server has not rendered anything for us yet. This
         * is the last step of setup and deserves closure: without it the panel
         * sits on the setup splash and looks like it never finished. */
        printf("paint: onboarded, no frame rendered yet\n");
        maybe_show_status(panel, variant, SPLASH_CONNECTED_NO_FRAME);
    } else if (skip_paint) {
        printf("paint: skipped (unchanged / none)\n");
    } else if (panel == NULL) {
        /* no driver */
    } else if (frame != NULL) {
        printf("painting downloaded frame (this takes ~20-45s)...\n");
        panel->paint(variant, frame);
        if (new_etag[0]) { config_set_frame_etag(new_etag); cfg_dirty = true; }
        note_frame_painted();
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

/*
 * One Tesserae cycle over the cloud relay (used when a relay pairing exists, or
 * a pairing is in flight). The relay is an ALTERNATIVE to the direct-REST path,
 * for a panel that cannot see the home instance at all; see relay.h.
 *
 * Shape mirrors do_cycle_rest: bootstrap if needed, conditional GET for the
 * frame, POST status, adopt config, drop the radio, then paint. The differences
 * are that every frame arrives sealed (and is decrypted in place in PSRAM), and
 * that config comes from a mailbox document rather than the status response.
 */
static bool do_cycle_relay(const panel_t *panel, uint8_t variant, size_t psram_sz)
{
    const config_t *c = config_get();
    bool cfg_dirty = false;
    bool reached   = false;

    /* Report the panel in its native landscape orientation, as the REST path
     * does: the pico_bin renderer packs 1600x1200 for the 13.3". */
    uint16_t pw = panel ? panel->width : 0, ph = panel ? panel->height : 0;
    if (pw < ph) { uint16_t t = pw; pw = ph; ph = t; }

    bool wifi_dirty = false;
    if (!config_has_wifi() ||
        !wifi_connect(c->wifi_ssid, c->wifi_pass, 20000, &wifi_dirty)) {
        printf("relay: wifi unavailable; skipping cycle\n");
        wifi_stop();                     /* still initialised on failure */
        if (wifi_dirty) config_save();
        maybe_show_status(panel, variant, SPLASH_WIFI_FAILED);
        return false;
    }
    if (wifi_dirty) cfg_dirty = true;

    /* 1. Finish pairing if it is still in flight. This can span several wakes:
     * the operator's code is submitted once, then polled until the home
     * instance completes the exchange on its side. */
    if (!relay_ready()) {
        relay_pair_result_t pr = relay_pair_step(pw, ph, &cfg_dirty);
        net_tls_deinit();   /* frees the parsed root store before we sleep/paint */
        wifi_stop();
        if (cfg_dirty) config_save();
        switch (pr) {
        case RELAY_PAIR_DONE:
            printf("relay: pairing complete; frames start next wake\n");
            return true;
        case RELAY_PAIR_WAITING:
            maybe_show_status(panel, variant, SPLASH_RELAY_PAIRING);
            return true;   /* we reached the relay: not a connectivity failure */
        case RELAY_PAIR_EXPIRED:
            /* The code is gone and single-use, so no amount of retrying helps.
             * Reopen setup, which is the only place a fresh one can be typed. */
            printf("relay: pairing expired; reopening setup\n");
            portal_run(panel, variant);   /* does not return */
            return true;
        default:
            maybe_show_status(panel, variant, SPLASH_SERVER_UNREACHABLE);
            return false;
        }
    }

    /* 2. Sealed frame, with If-None-Match against the last one we painted.
     * The buffer must hold the SEALED size: the nonce and GCM tag ride along
     * and the plaintext starts 12 bytes in, decrypted in place. */
    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    bool skip_paint = false;
    uint8_t *sram_buf = NULL;
    relay_frame_result_t fr = RELAY_FRAME_ERROR;

    if (panel != NULL) {
        uint32_t need = panel_frame_bytes(panel) + RELAY_SEAL_OVERHEAD;
        uint8_t *buf = NULL;
        if (need <= 256u * 1024) buf = sram_buf = malloc(need);
        if (buf == NULL && psram_sz >= need) buf = (uint8_t *)PSRAM_XIP_BASE;
        if (buf == NULL) {
            printf("relay: no buffer for %u sealed bytes\n", (unsigned)need);
        } else {
            fr = relay_fetch_frame(buf, need, &frame, &frame_len, &cfg_dirty);
            if (fr != RELAY_FRAME_ERROR) reached = true;
            if (fr == RELAY_FRAME_UNCHANGED || fr == RELAY_FRAME_NONE) {
                printf("relay: %s; skipping paint\n",
                       fr == RELAY_FRAME_UNCHANGED ? "frame unchanged (304)"
                                                   : "nothing published yet (204)");
                skip_paint = true;
                frame = NULL;
            } else if (fr == RELAY_FRAME_NEW) {
                /* Length is the real integrity check, not the relay's
                 * X-Tesserae-* headers: a frame that decrypts to the wrong size
                 * is for a different panel and would paint as a diagonal smear. */
                uint32_t want = panel_frame_bytes(panel);
                if (frame_len != want) {
                    printf("relay: decrypted frame is %u bytes, panel wants %u; dropping\n",
                           (unsigned)frame_len, (unsigned)want);
                    frame = NULL;
                }
            } else {
                frame = NULL;
            }
        }
    }

    /* 3. Telemetry, then the config document (which the status response tells
     * us whether we even need to fetch). */
    char ip[16];
    wifi_get_ip(ip, sizeof ip);
    if (relay_post_status(wifi_rssi(), ip, pw, ph, FW_VERSION)) reached = true;
    relay_sync_config(&cfg_dirty);

    /* A pairing the relay has disowned twice running is not coming back; keep
     * the current image and reopen setup so a fresh code can be typed.
     *
     * cfg_dirty must be forced: on a wake where every request 401'd, nothing
     * else dirtied the config, so without this the wipe would live only in RAM
     * and a power cycle before the operator finishes the form would restore the
     * dead pairing and start the two-wake revocation over again. */
    bool revoked = relay_pairing_revoked();
    if (revoked) {
        relay_forget_revoked_pairing();
        cfg_dirty = true;
    }

    net_tls_deinit();
    wifi_stop();   /* radio down before the slow refresh + flash write */

#ifdef DEV_SLEEP_S
    config_set_sleep_s(DEV_SLEEP_S);
#endif

    if (fr == RELAY_FRAME_NONE && config_get()->relay_etag[0] == '\0') {
        /* Paired, but home has not published a frame to the mailbox yet. Tested
         * BEFORE the generic skip_paint (which a 204 also sets), or setup never
         * gets its closure: the panel would sit on the portal's "Setup mode"
         * splash with no sign that pairing actually succeeded. Mirrors the REST
         * path's REST_NO_CONTENT branch. */
        printf("paint: paired, no frame published yet\n");
        maybe_show_status(panel, variant, SPLASH_CONNECTED_NO_FRAME);
    } else if (skip_paint) {
        printf("paint: skipped (unchanged / none)\n");
    } else if (frame != NULL) {
        printf("painting relay frame (this takes ~20-45s)...\n");
        panel->paint(variant, frame);
        relay_commit_frame(&cfg_dirty);
        note_frame_painted();
        printf("done.\n");
    } else if (revoked) {
        /* relay_forget_revoked_pairing() just blanked relay_etag, so the
         * "keeping last image" test below would read as "never painted" and
         * throw a 45 s onboarding refresh onto the glass moments before the
         * portal paints its own setup screen over it. Leave the dashboard up. */
        printf("paint: pairing revoked; keeping last image until setup\n");
    } else if (config_get()->relay_etag[0] != '\0') {
        printf("paint: no frame this cycle; keeping last image\n");
    } else if (reached) {
        /* Only when we actually got an answer from the relay. Without the
         * `reached` test a freshly paired panel on a network that cannot see
         * the relay would paint "Connected! Waiting for your first frame" --
         * the opposite of the truth -- and, being a status paint, would then
         * suppress run_cycle's correct "can't reach the server" message. */
        maybe_show_status(panel, variant, SPLASH_CONNECTED_NO_FRAME);
    }

    if (cfg_dirty) printf(config_save() ? "config: saved\n" : "config: SAVE FAILED\n");
    if (sram_buf != NULL) free(sram_buf);

    if (revoked) {
        printf("relay: reopening setup for a fresh pairing code\n");
        portal_run(panel, variant);   /* does not return */
    }
    return reached;
}

/* Run one cycle on the active transport, tracking connectivity. After
 * MAX_CONNECT_FAILS consecutive failures to reach WiFi/the server, re-open the
 * setup portal so a bad WiFi password or server URL can be corrected without a
 * reflash. The failure counter lives in always-on scratch (survives the
 * deep-sleep reboots between cycles). */
static void run_cycle(const panel_t *panel, uint8_t variant, size_t psram_sz)
{
    /* One status splash per cycle, not per boot: in production every cycle IS a
     * boot, but the dev loop (sleep_s <= 0) runs many cycles per power session
     * and would otherwise be stuck with whatever the first one decided. */
    s_status_splash_painted = false;

    /* Sample VSYS now, before either transport brings the radio up (the sense
     * pin GP43 is shared with the wireless). The heartbeat reads the cache. */
    battery_sample();
    printf("battery: %lu mV (~%d%%) [raw=%u]\n",
           (unsigned long)battery_mv(), battery_pct(), battery_raw());

    /* Transport selection, most specific first. A relay pairing (or a code in
     * flight) wins because a relay panel deliberately has NO server_url -- it
     * cannot see the home instance, which is the entire reason it is on the
     * relay. Then REST, then the legacy MQTT path. */
    bool reached;
    if (config_has_relay() && (relay_ready() || relay_pairing_pending()))
        reached = do_cycle_relay(panel, variant, psram_sz);
    else if (config_has_server())
        reached = do_cycle_rest(panel, variant, psram_sz);
    else
        reached = do_cycle(panel, variant, psram_sz);
    if (reached) { sleep_failcount_reset(); return; }

    /* Note: a pairing that is progressing normally never reaches here at all --
     * RELAY_PAIR_WAITING is reported as "reached", because getting an answer
     * from the relay is exactly what success looks like while home completes
     * its half. So the 24-hour patience costs nothing here, and the counter
     * still fires when the relay is genuinely UNREACHABLE. That distinction is
     * what keeps the setup portal available: suppressing the counter for any
     * pending pairing would lock the operator out for a day on a device whose
     * real problem is that it cannot reach the relay at all. */
    sleep_failcount_inc();
    uint32_t fails = sleep_failcount();
    printf("net: connectivity failure %u/%u\n", (unsigned)fails, MAX_CONNECT_FAILS);
    /* We got onto WiFi but never reached the server (a wrong URL, or the server
     * is down). Distinct from a WiFi failure and distinct from waiting on the
     * admin, because the fix is different in each case. This is the FALLBACK:
     * the cycle functions above paint the more specific message when they know
     * one, and maybe_show_status's one-per-cycle rule is what stops this
     * painting over it. */
    maybe_show_status(panel, variant, SPLASH_SERVER_UNREACHABLE);
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
    if (DEV_BOOT_LOG_DELAY_MS) sleep_ms(DEV_BOOT_LOG_DELAY_MS);   /* dev-only; 0 in prod */
    printf("\ntesserae-device-pico-bin " FW_VERSION "\n");

    /* Before anything else touches flash or the radio: the factory-reset hold.
     * It has to work on a device whose config is what is broken, so it must not
     * depend on config, WiFi or the panel being usable. */
    buttons_init();
    maybe_factory_reset();

#ifdef TESSERAE_TLS_DEBUG
    net_tls_paint_stack();   /* measure how deep the TLS path actually goes */
#endif


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
#ifdef SERVER_URL
    config_set_server(SERVER_URL);   /* selects the REST transport over MQTT */
#  ifdef PAIRING_CODE
    config_set_pairing_code(PAIRING_CODE);   /* optional; blank = discover/claim */
#  endif
#endif
#ifdef RELAY_URL
    config_set_relay_url(RELAY_URL);
#endif
#ifdef RELAY_CODE
    /* A relay code seeds a pairing attempt. Applied only when it differs from
     * what is already stored: re-applying it on every boot would restart the
     * pairing window each time (and, once paired, would clear a perfectly good
     * mailbox), so the code in secrets.h acts as a seed rather than a
     * standing instruction. */
    if (strcmp(config_get()->relay_code, RELAY_CODE) != 0 && !config_relay_ready()) {
        if (config_get()->relay_url[0] == '\0')
            config_set_relay_url(RELAY_DEFAULT_URL);
        config_clear_relay();
        config_set_relay_code(RELAY_CODE);
        printf("config: seeded relay pairing code from secrets.h\n");
    }
#endif
#ifdef ALWAYS_ON
    /* Seeded on EVERY boot, including with the value 0, because this is the
     * only thing that can clear a stored always_on. Guarding the write behind
     * "is it non-zero" would mean deleting the define leaves a device kiosked
     * forever with no way back short of a factory reset. */
    config_set_always_on(ALWAYS_ON);
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
    wake_reason_t wake = sleep_wake_reason();   /* also clears the across-wake counters on a cold boot */
    printf("wake: %s (boot #%u)\n",
           wake == WAKE_TIMER ? "timer" : "cold", (unsigned)sleep_boot_count());

    /* Status splashes may paint on a cold boot (which includes the reboot out of
     * the setup portal), or while the device has never shown a real frame. Once
     * a dashboard is up, a timer wake must not replace it with an error card
     * over one failed poll. See maybe_show_status(). */
    s_status_splash_allowed = (wake == WAKE_COLD) ||
                              (config_get()->last_frame_etag[0] == '\0' &&
                               config_get()->last_hash[0] == '\0' &&
                               config_get()->relay_etag[0] == '\0');

    /* Battery gate, before the cycle spends any power on radio or refresh. Does
     * not return on a flat cell. Reads the sample taken in run_cycle... which
     * has not happened yet, so take one here: the pin is free (the radio has
     * never been up this boot) and battery_sample() is idempotent. */
    battery_sample();
    battery_goodbye_check(panel, variant);

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

    /* Kiosk mode: hold the cycle open on a timer instead of deep-sleeping.
     * Worth being explicit about the cost -- the radio and the core stay
     * powered, so this is USB-power behaviour and would flatten a cell in
     * hours. It is never inferred; it comes from config.always_on, which is
     * either seeded from secrets.h or pushed by the server. */
    int32_t sleep_s = config_get()->sleep_s;
    if (!config_get()->always_on && sleep_s > 0) {
        uint32_t now = sleep_epoch_now();
        if (now) printf("sleep: until epoch %lu\n", (unsigned long)(now + sleep_s));
        printf("sleep: deep sleep for %ld s (wake reboots)\n", (long)sleep_s);
        sleep_deep_ms((uint64_t)sleep_s * 1000ull);   /* does not return */
    }

    /* Stay awake and loop. The interval is the server's own poll interval when
     * there is one, so a kiosk device refreshes at the same rate a sleeping one
     * would, rather than hammering the server at the dev default. */
    uint32_t loop_ms = (sleep_s > 0) ? (uint32_t)sleep_s * 1000u : DEV_LOOP_INTERVAL_MS;
    printf("awake: %s; re-checking every %u s\n",
           config_get()->always_on ? "always_on set" : "sleep_s<=0",
           (unsigned)(loop_ms / 1000u));
    while (true) {
        sleep_ms(loop_ms);
        run_cycle(panel, variant, psram_sz);
        /* Re-read each pass: the server can turn kiosk mode off mid-session,
         * and the next cycle should then go back to sleeping. */
        int32_t s = config_get()->sleep_s;
        if (!config_get()->always_on && s > 0) {
            printf("sleep: always_on cleared; deep sleep for %ld s\n", (long)s);
            sleep_deep_ms((uint64_t)s * 1000ull);   /* does not return */
        }
        loop_ms = (s > 0) ? (uint32_t)s * 1000u : DEV_LOOP_INTERVAL_MS;
    }
}
