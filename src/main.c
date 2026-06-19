/*
 * tesserae-device-pico-bin: MVP firmware.
 *
 * Paints one hardcoded test pattern, six vertical colour stripes, on a
 * Pimoroni Inky Impression 13.3" (EL133UF1 / Spectra 6) driven from a
 * Pimoroni Pico Plus 2. No networking, no MQTT, no sleep: this exists to
 * prove the panel paints what we tell it to from a known-good init sequence.
 *
 * Vertical stripes are a stronger proof than horizontal bands: the panel
 * splits left/right at column 600 across two controllers, so stripes 0..2
 * land on CS_M and stripes 3..5 on CS_S. If the split or a chip-select were
 * wrong, the seam at column 600 would show it.
 *
 * Stripe layout (1200 px wide / 6 = 200 px each):
 *   cols    0..199  black     |  cols  600..799  green   }
 *   cols  200..399  white     |  cols  800..999  blue    } CS_S (right half)
 *   cols  400..599  red       |  cols 1000..1199 yellow  }
 *   \---- CS_M (left half) ----/
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "epd_13in3e.h"
#include "epd_config.h"
#include "inky_eeprom.h"

/* The six stripe colours, left to right, as 4-bit palette nibbles. */
static const uint8_t kStripeColours[6] = {
    EPD_COL_BLACK, EPD_COL_WHITE,  EPD_COL_RED,
    EPD_COL_GREEN, EPD_COL_BLUE,   EPD_COL_YELLOW,
};

#define STRIPE_WIDTH_PX (EPD_WIDTH / 6)   /* 200 columns per stripe */

/* Map an absolute panel column (0..1199) to its stripe colour nibble. */
static inline uint8_t colour_at_column(int col)
{
    int stripe = col / STRIPE_WIDTH_PX;
    if (stripe > 5) stripe = 5;            /* guard the last few px if width %6 != 0 */
    return kStripeColours[stripe];
}

/*
 * Fill callback: produce one 300-byte half-row for the given controller side.
 * The stripe pattern is the same on every row, so `row` is unused; it stays in
 * the signature because real frames (a later layer) will vary by row.
 *
 * Byte b of a half-row covers two columns: the high nibble is the even column,
 * the low nibble the odd column. The left side starts at panel column 0, the
 * right side at column 600.
 */
static void fill_stripes(uint8_t *dst, enum epd_side side, int row)
{
    (void)row;
    int base_col = (side == EPD_SIDE_RIGHT) ? EPD_HALF_COLS : 0;   /* 600 or 0 */

    for (int b = 0; b < EPD_HALF_ROW_BYTES; b++) {
        int even_col = base_col + b * 2;
        int odd_col  = even_col + 1;
        dst[b] = EPD_PACK(colour_at_column(even_col), colour_at_column(odd_col));
    }
}

int main(void)
{
    stdio_init_all();

    /* Brief pause so a freshly attached USB serial monitor catches the logs.
     * The panel paints regardless of whether anyone is watching. */
    sleep_ms(2000);
    printf("\ntesserae-device-pico-bin: painting six vertical stripes\n");

    /* Read the model EEPROM first: a fast confirmation that the board is wired
     * and talking (over I2C, separate from the panel's SPI) before the slow
     * refresh. Not required for the paint; purely a sanity check. */
    inky_i2c_init();
    inky_eeprom_t ee;
    if (inky_eeprom_read(&ee)) {
        printf("eeprom: %ux%u variant=%u (%s)\n",
               ee.width, ee.height, ee.display_variant,
               inky_display_variant_name(ee.display_variant));
    } else {
        printf("eeprom: no response at 0x50 (continuing to paint anyway)\n");
    }

    epd_gpio_init();
    epd_panel_init();

    printf("epd: streaming frame...\n");
    epd_write_frame(fill_stripes);

    printf("epd: refreshing (this takes ~30s on Spectra 6)...\n");
    epd_panel_refresh();

    printf("done. the panel should now show: black white red | green blue yellow\n");

    while (true) {
        tight_loop_contents();
    }
}
