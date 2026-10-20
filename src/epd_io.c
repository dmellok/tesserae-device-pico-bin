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

void epd_board_init(void)
{
#ifdef INKY_FRAME_SPECTRA_7
    gpio_init(2); gpio_put(2, 1); gpio_set_dir(2, GPIO_OUT);
    /* PSRAM, SD card, display: all share SPI0. */
    const uint pins[] = {3, 22, 17};
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; ++i) {
        gpio_init(pins[i]);
        gpio_put(pins[i], 1);
        gpio_set_dir(pins[i], GPIO_OUT);
    }
#endif
}

#ifdef INKY_FRAME_SPECTRA_7
static uint8_t frame_shift_read(void)
{
    gpio_put(9, 0); sleep_us(1);
    gpio_put(9, 1); sleep_us(1);
    uint8_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value = (uint8_t)((value << 1) | gpio_get(10));
        gpio_put(8, 0); sleep_us(1);
        gpio_put(8, 1); sleep_us(1);
    }
    return value;
}
#endif

static bool epd_is_busy(void)
{
#ifdef INKY_FRAME_SPECTRA_7
    return (frame_shift_read() & 0x80) == 0;
#else
    return gpio_get(EPD_PIN_BUSY) == 0;
#endif
}

void epd_io_init(uint32_t spi_hz)
{
    spi_init(EPD_SPI, spi_hz);
    spi_set_format(EPD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);  /* mode 0 */
    gpio_set_function(EPD_PIN_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(EPD_PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(EPD_PIN_DC);  gpio_set_dir(EPD_PIN_DC, GPIO_OUT);  gpio_put(EPD_PIN_DC, 0);
    gpio_init(EPD_PIN_RST); gpio_set_dir(EPD_PIN_RST, GPIO_OUT); gpio_put(EPD_PIN_RST, 1);

#ifdef INKY_FRAME_SPECTRA_7
    gpio_init(8); gpio_put(8, 1); gpio_set_dir(8, GPIO_OUT);
    gpio_init(9); gpio_put(9, 1); gpio_set_dir(9, GPIO_OUT);
    gpio_init(10); gpio_set_dir(10, GPIO_IN);
#else
    gpio_init(EPD_PIN_BUSY);
    gpio_set_dir(EPD_PIN_BUSY, GPIO_IN);
    gpio_pull_up(EPD_PIN_BUSY);
#endif
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

bool epd_wait_ready(uint32_t timeout_ms)
{
#ifdef INKY_FRAME_SPECTRA_7
#ifdef EPD_DEBUG_SHIFT_REGISTER
    printf("epd: shift register=0x%02X before wait\n", frame_shift_read());
#endif
    absolute_time_t start = get_absolute_time();
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    sleep_ms(10);  /* Allow the controller to assert BUSY after a command. */
    while (epd_is_busy()) {
        if (time_reached(deadline)) {
            printf("epd: BUSY timeout after %ums\n", (unsigned)timeout_ms);
            return false;
        }
        sleep_ms(10);
    }
    printf("epd: ready after %lldms\n",
           (long long)(absolute_time_diff_us(start, get_absolute_time()) / 1000));
    return true;
#else
    /* Preserve the adapter timing for the existing Impression drivers. */
    sleep_ms(2000);
    uint32_t ms = 2000;
    while (epd_is_busy() && ms < timeout_ms) {
        sleep_ms(100);
        ms += 100;
    }
    bool ready = !epd_is_busy();
    printf("epd: %s after %ums\n", ready ? "ready" : "BUSY timeout", (unsigned)ms);
    return ready;
#endif
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
