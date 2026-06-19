/*
 * inky_eeprom.h: read the Inky Impression's model EEPROM over a bit-banged I2C
 * bus. Mirrors pimoroni/inky inky/eeprom.py.
 *
 * The EEPROM (I2C address 0x50, on the Pi I2C1 bus) stores the display variant,
 * so reading it identifies the panel and, just as usefully here, proves the
 * board is powered and reachable independently of the SPI panel path.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  color;
    uint8_t  pcb_variant;       /* stored x10; e.g. 12 means PCB rev 1.2 */
    uint8_t  display_variant;
    char     write_time[22];    /* EEPROM write timestamp (Pascal string) */
} inky_eeprom_t;

/* Configure the bit-banged I2C pins (call once before the read/scan calls). */
void inky_i2c_init(void);

/* Read and parse the EEPROM. Returns true if the device ACKed and was read. */
bool inky_eeprom_read(inky_eeprom_t *out);

/* Probe every 7-bit address and print which ones ACK. Diagnostic only. */
void inky_i2c_scan(void);

/* Human-readable name for a display_variant index, or "unknown". */
const char *inky_display_variant_name(uint8_t variant);
