/*
 * psram.h: RP2350 QMI bring-up for an APS6404 PSRAM (8 MB on the Pico Plus 2 W,
 * chip-select GP47). Ported from MicroPython's ports/rp2/rp2_psram.c.
 *
 * Only needed for frames too big for SRAM (the 13.3" panel's 960 KB buffer).
 * On a board without PSRAM, psram_init() returns 0 and the caller falls back to
 * SRAM (and refuses panels whose frame does not fit).
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "pico/stdlib.h"

#define PSRAM_CS_PIN_PLUS2   47u
#define PSRAM_XIP_BASE       ((uint8_t *)0x11000000u)   /* cached CS1 window     */
#define PSRAM_XIP_NOCACHE    ((uint8_t *)0x15000000u)   /* no-cache CS1 alias    */

/* Initialise the QMI for PSRAM on cs_pin. Returns detected size in bytes, or 0
 * if no PSRAM responds (board has none). Disables interrupts internally and
 * runs from RAM, so it is safe to call once early in main(). */
size_t psram_init(uint cs_pin);

/* Write/read-verify the whole PSRAM (call after a non-zero psram_init). */
bool psram_test(size_t size_bytes);
