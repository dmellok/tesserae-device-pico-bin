/*
 * epd_config.h: pin map, panel geometry, and colour palette for the
 * Pimoroni Inky Impression 13.3" (EL133UF1 / Spectra 6) driven from a
 * Pimoroni Pico Plus 2 (RP2350B).
 *
 * Edit pin numbers here, nowhere else.
 *
 * Wiring path: Pico Plus 2  ->  Hard Stuff "Pico to Pi Hat"  ->  Inky's
 * 40-pin Raspberry Pi header. Two layers of mapping decide each Pico GPIO:
 *
 *   1. The Inky driver fixes which Pi BCM line carries each panel signal.
 *      Source: pimoroni/inky, inky/inky_el133uf1.py
 *        CS0 = BCM26 (left half, cols 0..599)
 *        CS1 = BCM16 (right half, cols 600..1199)
 *        DC  = BCM22   RESET = BCM27   BUSY = BCM17
 *        MOSI = BCM10  SCLK  = BCM11
 *
 *   2. The adapter fixes which Pico GPIO each Pi BCM line lands on.
 *      Source: Hard Stuff "pico_to_pi_mappings.h" (thepihut.com product page)
 *        PI_MOSI (BCM10) -> GP36     PI_SCLK (BCM11) -> GP35
 *        PI_GPIO_26      -> GP7      PI_GPIO_16      -> GP42
 *        PI_GPIO_22      -> GP6      PI_GPIO_27      -> GP8
 *        PI_GPIO_17      -> GP41
 *
 * Composing the two layers gives the Pico GPIO assignments below.
 *
 * The Inky Impression has no software power-enable line, so unlike the
 * ESP32 port (which gated an EPD_PIN_PWR rail) there is no power pin here.
 */
#pragma once

#include <stdint.h>

/* ---- Pico Plus 2 GPIO assignments (BCM line in the trailing comment) ---- */
#define EPD_PIN_SCLK   35   /* clock   <- Pi SCLK / BCM11                     */
#define EPD_PIN_MOSI   36   /* data    <- Pi MOSI / BCM10                     */
#define EPD_PIN_CS_M    7   /* CS_M    <- Pi BCM26, left  half  cols   0..599 */
#define EPD_PIN_CS_S   42   /* CS_S    <- Pi BCM16, right half  cols 600..1199*/
#define EPD_PIN_DC      6   /* D/C     <- Pi BCM22 (low = command, high = data)*/
#define EPD_PIN_RST     8   /* reset   <- Pi BCM27 (active low pulse)         */
#define EPD_PIN_BUSY   41   /* busy    <- Pi BCM17 (input, pull-up; high = busy)*/

/*
 * GP35/GP36 are the only Pico pins the adapter wires to the panel's clock and
 * data lines, and on the RP2350B those pins are SPI0 TX and SPI0 RX (not SCK
 * and TX). No SPI peripheral can put a clock on GP35, so the hardware SPI
 * block cannot drive this panel through this adapter. We therefore bit-bang a
 * transmit-only SPI on plain GPIO. The panel is write-only here, and BUSY is a
 * separate GPIO, so nothing is lost. See epd_13in3e.c.
 *
 * GP35/36/41/42 only exist on the RP2350B (Pico Plus 2), which is exactly what
 * this adapter expects; a 30-pin Pico 2 would not expose them.
 */

/* ---- panel geometry (native portrait orientation) ---- */
#define EPD_WIDTH        1200
#define EPD_HEIGHT       1600
#define EPD_BPP          4                     /* packed 4 bits per pixel     */

/* Bytes per controller half-row. Each controller owns 600 columns; at 2
 * pixels per byte that is 300 bytes. The two halves are streamed separately,
 * one per chip-select. */
#define EPD_HALF_COLS    (EPD_WIDTH / 2)       /* 600 columns per controller  */
#define EPD_HALF_ROW_BYTES (EPD_HALF_COLS / 2) /* 300 bytes  (2 px / byte)    */

/* ---- 6-colour palette: 4-bit nibble values ---- */
/* Verbatim from the ESP32 port (include/app_config.h); 0x4 and 0x7 reserved. */
#define EPD_COL_BLACK    0x0
#define EPD_COL_WHITE    0x1
#define EPD_COL_YELLOW   0x2
#define EPD_COL_RED      0x3
#define EPD_COL_BLUE     0x5
#define EPD_COL_GREEN    0x6

/* Two pixels packed into one byte: high nibble is the even (leftmost) column,
 * low nibble the odd column. A run of one colour is (c << 4) | c. */
#define EPD_PACK(hi, lo) (uint8_t)(((hi) << 4) | ((lo) & 0x0F))
