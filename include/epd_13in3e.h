/*
 * epd_13in3e.h: driver for the Inky Impression 13.3" (EL133UF1 / Spectra 6).
 *
 * The init/power sequence, command values, BUSY polarity, and refresh steps
 * follow Pimoroni's Inky driver (pimoroni/inky, inky/inky_el133uf1.py), which
 * is the source of truth for this board. The Inky Impression is not a drop-in
 * for the bare Waveshare panel: it needs its own power configuration and uses
 * inverted BUSY, so the earlier Waveshare/ESP-IDF port (tesserae-device-esp32
 * -bin) left the panel blank. Here the Python reference's spidev/gpiod calls
 * become bit-banged GPIO on the RP2350.
 *
 * The panel has two controllers behind one SPI bus, selected by CS_M (left
 * half, columns 0..599) and CS_S (right half, columns 600..1199). A frame is
 * streamed to each controller in turn.
 *
 * Typical use:
 *     epd_gpio_init();
 *     epd_panel_init();
 *     epd_write_frame(my_fill_row);   // your pixels
 *     epd_panel_refresh();            // PON / DRF / POF; this is the slow part
 */
#pragma once

#include <stdint.h>

/* Which controller half a fill callback is being asked to produce. */
enum epd_side {
    EPD_SIDE_LEFT  = 0,   /* CS_M, columns   0..599  */
    EPD_SIDE_RIGHT = 1,   /* CS_S, columns 600..1199 */
};

/*
 * Fill callback. Called once per (side, row) while a frame is streamed. Write
 * exactly EPD_HALF_ROW_BYTES (300) bytes of packed 4bpp pixel data into dst:
 * the half-row for the given side and row (0..EPD_HEIGHT-1).
 *
 * The driver streams row by row and never holds a full framebuffer, so a
 * 1200x1600 4bpp image (960 KB, larger than RP2350 SRAM) never needs to exist
 * all at once.
 */
typedef void (*epd_fill_row_fn)(uint8_t *dst, enum epd_side side, int row);

/* Configure the GPIOs (call once at startup, before any other epd_ call). */
void epd_gpio_init(void);

/* Hardware reset followed by the panel init / power-config command sequence. */
void epd_panel_init(void);

/* Stream one full frame to both controllers using the fill callback. Does not
 * refresh the display; call epd_panel_refresh() afterwards to paint it. */
void epd_write_frame(epd_fill_row_fn fill);

/* Power on, refresh (paint the loaded frame), power off. Blocks on BUSY; a
 * full Spectra 6 refresh takes roughly 25-35 seconds. */
void epd_panel_refresh(void);
