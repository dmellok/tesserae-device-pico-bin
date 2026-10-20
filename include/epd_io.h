/*
 * epd_io.h: shared low-level transport for every Inky panel on this board.
 *
 * All the Inky Impression panels we support share the same control lines on the
 * Hard Stuff "Pico to Pi Hat" (a 1:1 BCM-to-GP passthrough that swaps clock and
 * data; see CLAUDE.md / README). Only the chip-select pin(s) differ per panel,
 * so CS is passed in by each panel rather than fixed here.
 *
 *   SCLK -> GP10 (SPI1 SCK)   MOSI -> GP11 (SPI1 TX)
 *   DC   -> GP22              RST  -> GP27              BUSY -> GP17 (LOW = busy)
 *
 * BUSY is active low on these panels (Waveshare convention): the controller
 * holds it low while refreshing and releases it high when done.
 *
 * INKY_FRAME_SPECTRA_7 builds for the Inky Frame 7.3" Spectra 6 with its
 * Pico 2 W aboard instead. The panel shares SPI0 with the Frame's PSRAM and
 * SD card, and BUSY is not on a GPIO but on bit 7 of the Frame's shift
 * register (LOW = busy):
 *
 *   SCLK -> GP18 (SPI0 SCK)   MOSI -> GP19 (SPI0 TX)    CS  -> GP17
 *   DC   -> GP28              RST  -> GP27
 *   BUSY -> shift register bit 7 (clock GP8, latch GP9, data GP10)
 *   GP2 holds battery power on; PSRAM CS GP3 and SD CS GP22 are held high.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "hardware/spi.h"
#include "pico/stdlib.h"

/* Board wiring is independent of the EEPROM panel variant. */
#ifdef INKY_FRAME_SPECTRA_7
#define EPD_SPI        spi0
#define EPD_PIN_SCLK   18   /* SPI0 SCK                                      */
#define EPD_PIN_MOSI   19   /* SPI0 TX                                       */
#define EPD_PIN_DC     28   /* low = command, high = data                    */
#define EPD_PIN_RST    27   /* active low                                    */
#define EPD_SMALL_CS   17   /* panel chip select; BUSY is via shift register */
#else
#define EPD_SPI        spi1
#define EPD_PIN_SCLK   10   /* SPI1 SCK; adapter routes Pi SCLK (BCM11) here */
#define EPD_PIN_MOSI   11   /* SPI1 TX;  adapter routes Pi MOSI (BCM10) here */
#define EPD_PIN_DC     22   /* low = command, high = data                    */
#define EPD_PIN_RST    27   /* active low                                    */
#define EPD_PIN_BUSY   17   /* input, pull-up; LOW = busy, HIGH = ready       */
#define EPD_SMALL_CS   8    /* single-controller panels (E673, E640, 7-col)  */
#endif

/* Call before other startup work to hold Frame battery power on and deselect
 * devices on its shared SPI bus. No-op for the Impression adapter. */
void epd_board_init(void);

/* Configure the selected SPI bus + DC/RST/BUSY. Call once at the start of a
 * panel's run, with that panel's SPI clock (panels differ: ~1-5 MHz). */
void epd_io_init(uint32_t spi_hz);

/* Configure a chip-select pin as an output, idle high (deasserted). */
void epd_cs_init(uint cs_pin);

/* Hardware reset: `pulses` low/high cycles (low_ms low, high_ms high), then
 * hold high and settle for settle_ms. RST is active low. */
void epd_reset(int pulses, uint32_t low_ms, uint32_t high_ms, uint32_t settle_ms);

/* Wait for BUSY to clear. Returns false and logs a timeout on failure.
 * Frame reads shift-register bit 7, with 10ms command settling time.
 * The Impression profile retains its original two-second settling delay. */
bool epd_wait_ready(uint32_t timeout_ms);

/* Send one command, with optional trailing data, to the given chip-select
 * line(s) (all asserted together, active low, for the whole transaction).
 * setup_ms is the D/C-to-clock hold the controller needs before the opcode
 * (300 for Spectra 6 panels; 0 for the 7-colour panels). */
void epd_command(const uint8_t *cs_pins, uint8_t n_cs, uint32_t setup_ms,
                 uint8_t cmd, const uint8_t *data, size_t len);

/* Stream a frame to one controller: open a data-transfer command, push bytes
 * (call write as many times as needed), then close. */
void epd_dtm_begin(uint8_t cs_pin, uint32_t setup_ms, uint8_t dtm_cmd);
void epd_dtm_write(const uint8_t *data, size_t len);
void epd_dtm_end(void);
