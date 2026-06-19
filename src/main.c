/*
 * tesserae-device-pico-bin: MVP firmware.
 *
 * Detects the attached Pimoroni Inky panel from its model EEPROM and paints a
 * hardcoded test pattern (vertical colour stripes) using the matching panel
 * driver. No networking, no sleep, no Tesserae integration yet: this proves the
 * panel paints what we tell it to.
 *
 * Only the 13.3" (EL133UF1) is verified on hardware; the other panel drivers
 * are blind ports from the Pimoroni reference and are flagged UNTESTED.
 */
#include <stdio.h>
#include "pico/stdlib.h"

#include "inky_eeprom.h"
#include "panels.h"
#include "net_wifi.h"
#include "config.h"

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);   /* let a freshly attached USB serial monitor catch the logs */
    printf("\ntesserae-device-pico-bin\n");

    /* Phase 2 (WIP, building toward ESP32-client parity): load persistent
     * config from flash. On first boot (blank flash) seed it from secrets.h if
     * present, then save. Radio is down here, so the flash write is safe. */
    bool had_config = config_load();
    if (!had_config) {
        printf("config: none saved; initialising\n");
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
        printf(config_save() ? "config: saved (seeded from secrets.h where set)\n"
                             : "config: SAVE FAILED\n");
    } else {
        printf("config: loaded from flash\n");
    }
    const config_t *c = config_get();
    printf("config: wifi_ssid='%s' device_id='%s' sleep_s=%ld last_hash=%s\n",
           c->wifi_ssid, c->mqtt_device_id[0] ? c->mqtt_device_id : "(default)",
           (long)c->sleep_s, c->last_hash[0] ? "set" : "(none)");

    /* Connectivity check using stored creds (radio powered back down after). */
    if (config_has_wifi()) {
        if (wifi_connect(c->wifi_ssid, c->wifi_pass, 20000)) wifi_stop();
    } else {
        printf("wifi: no SSID configured; skipping (portal comes in a later phase)\n");
    }

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
    if (panel == NULL) {
        printf("no driver for variant %u; nothing to paint\n", variant);
        while (true) tight_loop_contents();
    }

    printf("panel: %s (%ux%u)%s\n", panel->name, panel->width, panel->height,
           panel->verified ? "" : "  [UNTESTED driver]");
    printf("painting vertical colour stripes (this takes ~20-45s)...\n");
    panel->run(variant);
    printf("done.\n");

    while (true) tight_loop_contents();
}
