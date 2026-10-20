/* Shared display transport. INKY_FRAME_SPECTRA_7 selects the Pico 2 W
 * aboard Frame wiring; otherwise use the Pico Plus 2 W Impression adapter.
 * Frame BUSY is shift-register bit 7; Impression BUSY is direct GP17.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "hardware/spi.h"
#include "pico/stdlib.h"

/* Board wiring is independent of the EEPROM panel variant. */
#ifdef INKY_FRAME_SPECTRA_7
#define EPD_SPI        spi0
#define EPD_PIN_SCLK   18
#define EPD_PIN_MOSI   19
#define EPD_PIN_DC     28
#define EPD_PIN_RST    27
#define EPD_SMALL_CS   17
#else
#define EPD_SPI        spi1
#define EPD_PIN_SCLK   10
#define EPD_PIN_MOSI   11
#define EPD_PIN_DC     22
#define EPD_PIN_RST    27
#define EPD_PIN_BUSY   17
#define EPD_SMALL_CS   8
#endif

/* Call before other startup work to hold Frame battery power on and deselect
 * devices on its shared SPI bus. No-op for the Impression adapter. */
void epd_board_init(void);

/* Configure the selected SPI bus + DC/RST/BUSY. Call once at the start of a panel's run, with
 * that panel's SPI clock (panels differ: ~1-5 MHz). */
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
