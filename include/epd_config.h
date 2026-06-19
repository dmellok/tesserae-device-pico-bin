/*
 * epd_config.h: pin map, panel geometry, and colour palette for the
 * Pimoroni Inky Impression 13.3" (EL133UF1 / Spectra 6) driven from a
 * Pimoroni Pico Plus 2 (RP2350B).
 *
 * Edit pin numbers here, nowhere else.
 *
 * Wiring path: Pico Plus 2 W  ->  Hard Stuff "Pico to Pi Hat"  ->  Inky's
 * 40-pin Raspberry Pi header.
 *
 * The adapter on this unit is almost a 1:1 passthrough: each Raspberry Pi BCM
 * line connects to the Pico GPIO of the SAME number (verified by continuity:
 * GP18<->BCM18, GP19<->BCM19, BCM26<->GP26, ...), WITH ONE EXCEPTION: it swaps
 * clock and data. SCLK (Pi BCM11) lands on Pico GP10, and MOSI (Pi BCM10) lands
 * on Pico GP11 (both confirmed by meter). So the Pico must drive the clock on
 * GP10 and data on GP11. The vendor's pico_to_pi_mappings.h / PDF do not match
 * this board at all; the meter is the source of truth.
 *
 * Inky Pi BCM lines (from inky/inky_el133uf1.py):
 *   CS0 = BCM26 (left half, cols 0..599)    CS1 = BCM16 (right half)
 *   DC  = BCM22   RESET = BCM27   BUSY = BCM17
 *   MOSI = BCM10  SCLK  = BCM11
 *
 * The Inky Impression has no software power-enable line, so there is no power
 * pin here; the panel takes 3V3 and GND from the Pico.
 */
#pragma once

#include "hardware/spi.h"
#include <stdint.h>

/* Hardware SPI block. After the adapter's clock/data swap, SCLK lands on GP10
 * (SPI1 SCK) and MOSI on GP11 (SPI1 TX), which is a valid SPI1 pin pair, so we
 * use the hardware peripheral instead of bit-banging. */
#define EPD_SPI      spi1
#define EPD_SPI_HZ   (4 * 1000 * 1000)   /* 4 MHz, comfortably under the panel max */

/* ---- Pico Plus 2 GPIO assignments (1:1 with Pi BCM, except SCLK/MOSI) ---- */
#define EPD_PIN_SCLK   10   /* clock: GP10 -> Pi BCM11 (SCLK). ADAPTER SWAPS   */
#define EPD_PIN_MOSI   11   /* data:  GP11 -> Pi BCM10 (MOSI). these two!      */
#define EPD_PIN_CS_M   26   /* CS_M  <- Pi BCM26, left  half  cols   0..599    */
#define EPD_PIN_CS_S   16   /* CS_S  <- Pi BCM16, right half  cols 600..1199   */
#define EPD_PIN_DC     22   /* D/C   <- Pi BCM22 (low = command, high = data)  */
#define EPD_PIN_RST    27   /* reset <- Pi BCM27 (active low pulse)            */
#define EPD_PIN_BUSY   17   /* busy  <- Pi BCM17 (input, pull-up; LOW = busy)  */

/* Inky model EEPROM (I2C 0x50), on Pi SDA1/SCL1 = BCM2/BCM3 -> GP2/GP3.
 * Bit-banged; a quick comms check and model read. */
#define EPD_PIN_SDA     2
#define EPD_PIN_SCL     3

/*
 * SCLK=GP11 and MOSI=GP10 are SPI1 TX and SPI1 SCK respectively, i.e. the
 * roles are swapped relative to what the hardware SPI block needs, so we
 * bit-bang SPI on plain GPIO (the panel is write-only; see epd_13in3e.c).
 * None of these pins collide with the RM2 wireless (GP23/24/25/29).
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
