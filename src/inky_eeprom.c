/*
 * inky_eeprom.c: bit-banged I2C master and Inky model-EEPROM reader.
 *
 * Mirrors pimoroni/inky inky/eeprom.py: address 0x50, set the word-address
 * pointer to 0x0000 with a dummy write, then read 29 bytes laid out as the
 * struct "<HHBBB22p" (width, height, color, pcb_variant, display_variant, and
 * a 22-byte Pascal-string timestamp).
 *
 * I2C is bit-banged because the adapter routes the Pi I2C lines to GP13/GP14,
 * which are not a usable hardware-I2C pair on the RP2350B (see epd_config.h).
 * SCL is driven push-pull (the EEPROM does not clock-stretch); SDA is
 * open-drain, emulated by switching the pin between output-low and input
 * (released, pulled high). ~100 kHz, which is comfortable for a 24Cxx EEPROM.
 */
#include "inky_eeprom.h"
#include "epd_config.h"

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define EEP_ADDR     0x50
/* Half bit period. Deliberately slow (~10 kHz) so even the weak internal
 * pull-ups can pull the lines high in time; a powered Inky adds stronger
 * external pull-ups, but running slow removes rise-time as a variable while
 * diagnosing. Drop this once the bus is confirmed working. */
#define I2C_HALF_US  50

/* SDA open-drain: drive low, or release (input) and let the pull-up raise it. */
static inline void sda_low(void)    { gpio_set_dir(EPD_PIN_SDA, GPIO_OUT); }   /* output value preset to 0 */
static inline void sda_release(void){ gpio_set_dir(EPD_PIN_SDA, GPIO_IN);  }
static inline bool sda_read(void)   { gpio_set_dir(EPD_PIN_SDA, GPIO_IN); return gpio_get(EPD_PIN_SDA); }
static inline void scl_set(int hi)  { gpio_put(EPD_PIN_SCL, hi); }
static inline void half(void)       { sleep_us(I2C_HALF_US); }

void inky_i2c_init(void)
{
    gpio_init(EPD_PIN_SDA);
    gpio_init(EPD_PIN_SCL);

    /* SDA: preset output level low, start released (high via pull-up). */
    gpio_put(EPD_PIN_SDA, 0);
    gpio_set_dir(EPD_PIN_SDA, GPIO_IN);
    gpio_pull_up(EPD_PIN_SDA);

    /* SCL: push-pull output, idle high. */
    gpio_set_dir(EPD_PIN_SCL, GPIO_OUT);
    gpio_put(EPD_PIN_SCL, 1);
    gpio_pull_up(EPD_PIN_SCL);
}

static void i2c_start(void)
{
    sda_release(); scl_set(1); half();
    sda_low();     half();
    scl_set(0);    half();
}

static void i2c_stop(void)
{
    sda_low();     half();
    scl_set(1);    half();
    sda_release(); half();
}

/* Clock out 8 bits MSB-first, then sample the ACK. Returns true on ACK. */
static bool i2c_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) sda_release(); else sda_low();
        b <<= 1;
        half(); scl_set(1); half(); scl_set(0); half();
    }
    sda_release();                 /* let the slave drive ACK */
    half(); scl_set(1); half();
    bool ack = (sda_read() == 0);  /* slave pulls SDA low to ACK */
    scl_set(0); half();
    return ack;
}

/* Clock in 8 bits MSB-first, then send ACK (more to follow) or NACK (last). */
static uint8_t i2c_read_byte(bool ack)
{
    uint8_t b = 0;
    sda_release();
    for (int i = 0; i < 8; i++) {
        half(); scl_set(1); half();
        b = (uint8_t)((b << 1) | (sda_read() ? 1 : 0));
        scl_set(0); half();
    }
    if (ack) sda_low(); else sda_release();
    half(); scl_set(1); half(); scl_set(0); half();
    sda_release();
    return b;
}

bool inky_eeprom_read(inky_eeprom_t *out)
{
    uint8_t d[29];

    /* Dummy write: set the EEPROM's word-address pointer to 0x0000. */
    i2c_start();
    if (!i2c_write_byte(EEP_ADDR << 1)) { i2c_stop(); return false; }  /* no device */
    i2c_write_byte(0x00);
    i2c_write_byte(0x00);
    i2c_stop();

    /* Read 29 bytes from the current pointer. */
    i2c_start();
    if (!i2c_write_byte((EEP_ADDR << 1) | 1)) { i2c_stop(); return false; }
    for (int i = 0; i < 29; i++) {
        d[i] = i2c_read_byte(i < 28);   /* ACK all but the final byte */
    }
    i2c_stop();

    out->width           = (uint16_t)(d[0] | (d[1] << 8));
    out->height          = (uint16_t)(d[2] | (d[3] << 8));
    out->color           = d[4];
    out->pcb_variant     = d[5];
    out->display_variant = d[6];

    uint8_t slen = d[7];               /* Pascal string length byte */
    if (slen > 21) slen = 21;
    memcpy(out->write_time, &d[8], slen);
    out->write_time[slen] = '\0';
    return true;
}

void inky_i2c_scan(void)
{
    printf("i2c: scanning bus (SDA=GP%d, SCL=GP%d)...\n", EPD_PIN_SDA, EPD_PIN_SCL);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        i2c_start();
        bool ack = i2c_write_byte((uint8_t)(a << 1));
        i2c_stop();
        if (ack) {
            printf("i2c:   device ACK at 0x%02X\n", a);
            found++;
        }
        sleep_ms(1);
    }
    printf("i2c: %d device(s) responded\n", found);
}

/* Names indexed by display_variant, copied from inky/eeprom.py. */
static const char *const kVariantNames[] = {
    NULL,
    "Red pHAT (High-Temp)",
    "Yellow wHAT",
    "Black wHAT",
    "Black pHAT",
    "Yellow pHAT",
    "Red wHAT",
    "Red wHAT (High-Temp)",
    "Red wHAT",
    NULL,
    "Black pHAT (SSD1608)",
    "Red pHAT (SSD1608)",
    "Yellow pHAT (SSD1608)",
    NULL,
    "7-Colour (UC8159)",
    "7-Colour 640x400 (UC8159)",
    "7-Colour 640x400 (UC8159)",
    "Black wHAT (SSD1683)",
    "Red wHAT (SSD1683)",
    "Yellow wHAT (SSD1683)",
    "7-Colour 800x480 (AC073TC1A)",
    "Spectra 6 13.3 1600 x 1200 (EL133UF1)",
    "Spectra 6 7.3 800 x 480 (E673)",
    "Red/Yellow pHAT (JD79661)",
    "Red/Yellow wHAT (JD79668)",
    "Spectra 6 4.0 600 x 400 (E640)",
    "Spectra 6 7.3 800 x 480 (E673) AC",
    "Spectra 6 13.3 1600 x 1200 (EL133UF1) AC",
};

const char *inky_display_variant_name(uint8_t variant)
{
    size_t n = sizeof(kVariantNames) / sizeof(kVariantNames[0]);
    if (variant >= n || kVariantNames[variant] == NULL) {
        return "unknown";
    }
    return kVariantNames[variant];
}
