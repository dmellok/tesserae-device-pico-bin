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
#include "net_http.h"
#include "net_sntp.h"
#include "config.h"
#include "psram.h"
#include "sleepmgr.h"

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
static void do_cycle(const panel_t *panel, uint8_t variant, size_t psram_sz)
{
    const config_t *c = config_get();
    const uint8_t *frame      = NULL;   /* what we'll paint; NULL -> stripe test pattern */
    uint8_t       *sram_buf   = NULL;   /* malloc'd buffer to free after paint, if any   */
    bool           skip_paint = false;
    bool           cfg_dirty  = false;
    char           new_hash[65] = {0};

    if (config_has_wifi() && wifi_connect(c->wifi_ssid, c->wifi_pass, 20000)) {
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
                 "{\"fw_version\":\"0.1.0-pico\",\"kind\":\"pico_bin_client\","
                 "\"battery_mv\":0,\"battery_pct\":0,"
                 "\"rssi\":%d,\"ip\":\"%s\",\"panel_w\":%u,\"panel_h\":%u,"
                 "\"sleep_interval_s\":%ld,\"next_sleep_s\":%ld,"
                 "\"wake_reason\":\"%s\",\"boot\":%u",
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
    wake_reason_t wake = sleep_wake_reason();
    printf("wake: %s (boot #%u)\n",
           wake == WAKE_TIMER ? "timer" : "cold", (unsigned)sleep_boot_count());

    /* Run one cycle, then either deep-sleep until the next refresh or, in dev
     * mode (sleep_s <= 0), stay awake and loop so the serial monitor survives
     * and the device can be reflashed. sleep_s may be updated by the cycle. */
    do_cycle(panel, variant, psram_sz);

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
        do_cycle(panel, variant, psram_sz);
    }
}
