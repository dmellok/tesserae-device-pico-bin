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
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define EPD_SPI        spi1
#define EPD_PIN_SCLK   10   /* SPI1 SCK; adapter routes Pi SCLK (BCM11) here */
#define EPD_PIN_MOSI   11   /* SPI1 TX;  adapter routes Pi MOSI (BCM10) here */
#define EPD_PIN_DC     22   /* low = command, high = data                    */
#define EPD_PIN_RST    27   /* active low                                    */
#define EPD_PIN_BUSY   17   /* input, pull-up; LOW = busy, HIGH = ready       */

/* Configure SPI1 + DC/RST/BUSY. Call once at the start of a panel's run, with
 * that panel's SPI clock (panels differ: ~1-5 MHz). */
void epd_io_init(uint32_t spi_hz);

/* Configure a chip-select pin as an output, idle high (deasserted). */
void epd_cs_init(uint cs_pin);

/* Hardware reset: `pulses` low/high cycles (low_ms low, high_ms high), then
 * hold high and settle for settle_ms. RST is active low. */
void epd_reset(int pulses, uint32_t low_ms, uint32_t high_ms, uint32_t settle_ms);

/* Wait while BUSY is low (busy); return when it goes high (ready) or after
 * timeout_ms. Settles briefly first so a just-issued refresh has asserted BUSY
 * before we sample it. */
void epd_wait_ready(uint32_t timeout_ms);

/* Outcome of waiting on a display refresh (DRF). */
typedef enum {
    EPD_REFRESH_OK = 0,       /* BUSY asserted, then released: the refresh ran */
    EPD_REFRESH_NOT_STARTED,  /* BUSY never went low: the controller ignored DRF */
    EPD_REFRESH_TIMEOUT,      /* BUSY asserted but never released in time */
} epd_refresh_t;

/* Wait for a just-issued refresh, in two distinct phases.
 *
 * This exists because epd_wait_ready() cannot tell "the refresh finished" from
 * "the refresh never started". Both leave BUSY high, and the caller's next move
 * is power-off (POF) -- so a DRF the controller dropped (which happens when the
 * D/C setup hold is marginal; see DC_SETUP_MS in CLAUDE.md) gets the panel
 * powered down mid-nothing and leaves the glass blank or half-written, with a
 * cheerful "refresh done" in the log.
 *
 * Phase 1 waits up to start_ms for BUSY to go LOW, which is the controller
 * acknowledging DRF. Phase 2 then waits up to timeout_ms for it to go back
 * HIGH. A NOT_STARTED result is worth logging loudly and is the caller's cue
 * that the frame did not reach the glass. */
epd_refresh_t epd_wait_refresh(uint32_t start_ms, uint32_t timeout_ms);

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
