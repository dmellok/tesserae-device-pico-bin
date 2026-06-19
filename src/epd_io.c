/*
 * epd_io.c: shared SPI/GPIO transport for the Inky panels. See epd_io.h.
 */
#include "epd_io.h"

#include <stdio.h>
#include "hardware/gpio.h"

static uint8_t s_dtm_cs;   /* CS held during the current epd_dtm_* sequence */

static inline void spi_tx(const uint8_t *data, size_t len)
{
    spi_write_blocking(EPD_SPI, data, len);
}

void epd_io_init(uint32_t spi_hz)
{
    spi_init(EPD_SPI, spi_hz);
    spi_set_format(EPD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);  /* mode 0 */
    gpio_set_function(EPD_PIN_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(EPD_PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(EPD_PIN_DC);  gpio_set_dir(EPD_PIN_DC, GPIO_OUT);  gpio_put(EPD_PIN_DC, 0);
    gpio_init(EPD_PIN_RST); gpio_set_dir(EPD_PIN_RST, GPIO_OUT); gpio_put(EPD_PIN_RST, 1);

    gpio_init(EPD_PIN_BUSY);
    gpio_set_dir(EPD_PIN_BUSY, GPIO_IN);
    gpio_pull_up(EPD_PIN_BUSY);
}

void epd_cs_init(uint cs_pin)
{
    gpio_init(cs_pin);
    gpio_set_dir(cs_pin, GPIO_OUT);
    gpio_put(cs_pin, 1);   /* idle high (deasserted) */
}

void epd_reset(int pulses, uint32_t low_ms, uint32_t high_ms, uint32_t settle_ms)
{
    for (int i = 0; i < pulses; i++) {
        gpio_put(EPD_PIN_RST, 0); sleep_ms(low_ms);
        gpio_put(EPD_PIN_RST, 1); sleep_ms(high_ms);
    }
    sleep_ms(settle_ms);
}

void epd_wait_ready(uint32_t timeout_ms)
{
    sleep_ms(2000);                         /* let a just-issued op assert BUSY low */
    uint32_t ms = 2000;
    while (gpio_get(EPD_PIN_BUSY) == 0 && ms < timeout_ms) {   /* 0 = busy */
        sleep_ms(100);
        ms += 100;
    }
    printf("epd: ready after %ums (busy=%d)\n", (unsigned)ms, gpio_get(EPD_PIN_BUSY));
}

static inline void cs_assert(const uint8_t *cs_pins, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) gpio_put(cs_pins[i], 0);
}

static inline void cs_release(const uint8_t *cs_pins, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) gpio_put(cs_pins[i], 1);
}

void epd_command(const uint8_t *cs_pins, uint8_t n_cs, uint32_t setup_ms,
                 uint8_t cmd, const uint8_t *data, size_t len)
{
    cs_assert(cs_pins, n_cs);
    gpio_put(EPD_PIN_DC, 0);          /* command */
    sleep_ms(setup_ms);
    spi_tx(&cmd, 1);
    if (data != NULL && len > 0) {
        gpio_put(EPD_PIN_DC, 1);      /* data */
        spi_tx(data, len);
    }
    cs_release(cs_pins, n_cs);
}

void epd_dtm_begin(uint8_t cs_pin, uint32_t setup_ms, uint8_t dtm_cmd)
{
    s_dtm_cs = cs_pin;
    gpio_put(cs_pin, 0);
    gpio_put(EPD_PIN_DC, 0);
    sleep_ms(setup_ms);
    spi_tx(&dtm_cmd, 1);
    gpio_put(EPD_PIN_DC, 1);          /* data follows */
}

void epd_dtm_write(const uint8_t *data, size_t len)
{
    spi_tx(data, len);
}

void epd_dtm_end(void)
{
    gpio_put(s_dtm_cs, 1);
}
