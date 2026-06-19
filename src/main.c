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

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);   /* let a freshly attached USB serial monitor catch the logs */
    printf("\ntesserae-device-pico-bin\n");

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
