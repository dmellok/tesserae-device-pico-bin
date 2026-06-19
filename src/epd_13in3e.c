/*
 * epd_13in3e.c: Inky Impression 13.3" (EL133UF1 / Spectra 6) driver.
 *
 * The init/power command sequence and argument values follow Pimoroni's Inky
 * driver, inky/inky_el133uf1.py (the source of truth for this board). An
 * earlier version ported the Waveshare ESP32 demo and the panel stayed blank:
 * the Inky needs a different power configuration (extra DCDC / POFS / CMDA4
 * commands, different boost and PSR/CDI values), so it is not a drop-in.
 *
 * Transport: SCLK/MOSI are driven by the RP2350B hardware SPI block (SPI1, see
 * epd_config.h); CS, D/C, and reset are plain GPIO; BUSY is a GPIO input. The
 * panel is write-only (no MISO).
 *
 * Two things that cost real debugging time and are easy to get wrong:
 *   1. Command setup delay. The panel will not latch a command unless D/C is
 *      held for a good while before the clock starts. The Pimoroni driver waits
 *      300 ms (DC_SETUP_MS below); shorter values leave the panel ignoring the
 *      DTM (data) command, so it refreshes an empty buffer and stays blank.
 *   2. BUSY polarity is LOW = busy on this panel (Waveshare convention), the
 *      OPPOSITE of what the Pimoroni Python code's pull-up heuristic implies.
 *      See wait_refresh_done().
 */
#include "epd_13in3e.h"
#include "epd_config.h"

#include <stddef.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

/* --- panel command opcodes (EL133UF1) --- */
#define PSR             0x00   /* panel setting                            */
#define PWR             0x01   /* power setting                            */
#define POF             0x02   /* power off                                */
#define POFS            0x03   /* power off sequence (per controller)      */
#define PON             0x04   /* power on                                 */
#define BTST_N          0x05   /* booster soft-start, negative             */
#define BTST_P          0x06   /* booster soft-start, positive             */
#define DTM             0x10   /* data transfer (frame data)               */
#define DRF             0x12   /* display refresh (paints the loaded frame)*/
#define PLL             0x30   /* PLL control                              */
#define CDI             0x50   /* VCOM and data interval                   */
#define TCON            0x60   /* gate/source timing                       */
#define TRES            0x61   /* resolution setting                       */
#define ANTM            0x74   /* analogue timing (master-only)            */
#define AGID            0x86   /* gate/source independent driving          */
#define CMDA4           0xA4   /* vendor power/sequence config             */
#define DCDC            0xA5   /* DC/DC converter config                   */
#define BUCK_BOOST_VDDN 0xB0
#define TFT_VCOM_POWER  0xB1
#define EN_BUF          0xB6
#define BOOST_VDDP_EN   0xB7
#define CCSET           0xE0
#define PWS             0xE3   /* power saving                             */
#define CMD66           0xF0

/* Chip-select masks. CS0 = CS_M (left half, cols 0..599); CS1 = CS_S (right
 * half, cols 600..1199). Mirrors CS0_SEL/CS1_SEL in the Pimoroni driver. */
#define CS0_SEL  0x01
#define CS1_SEL  0x02
#define CS_BOTH  (CS0_SEL | CS1_SEL)

/*
 * Setup delay after asserting CS and driving D/C low, before clocking the
 * command byte. This is NOT optional padding: with a short delay the panel
 * misses commands (notably DTM), refreshes an empty buffer, and stays blank.
 * 300 ms matches the Pimoroni driver and is what made the panel paint here.
 * It runs once per command (~26 commands), so it adds ~8 s to a refresh; that
 * is the price of reliable command latching on this controller.
 */
#define DC_SETUP_MS 300

/* --- canned command parameter blobs (from inky_el133uf1.py; do NOT edit) --- */
static const uint8_t ANTM_V[]            = {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55};
static const uint8_t CMD66_V[]           = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t PSR_V[]             = {0xDF, 0x6B};
static const uint8_t DCDC_V[]            = {0x44, 0x54, 0x00};
static const uint8_t PLL_V[]             = {0x08};
static const uint8_t CDI_V[]             = {0x37};
static const uint8_t TCON_V[]            = {0x03, 0x03};
static const uint8_t POFS_CS0_V[]        = {0x00, 0xC0, 0x03, 0xA8};
static const uint8_t POFS_CS1_V[]        = {0x00, 0xC0, 0x03, 0x9A};
static const uint8_t AGID_V[]            = {0x10};
static const uint8_t PWS_V[]             = {0x22};
static const uint8_t CCSET_V[]           = {0x01};
static const uint8_t TRES_V[]            = {0x04, 0xB0, 0x03, 0x20};
static const uint8_t CMDA4_V[]           = {0x03, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00};
static const uint8_t PWR_V[]             = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t EN_BUF_V[]          = {0x07};
static const uint8_t BTST_P_V[]          = {0xE0, 0x20};
static const uint8_t BOOST_VDDP_EN_V[]   = {0x01};
static const uint8_t BTST_N_V[]          = {0xE0, 0x20};
static const uint8_t BUCK_BOOST_VDDN_V[] = {0x01};
static const uint8_t TFT_VCOM_POWER_V[]  = {0x02};
static const uint8_t DRF_V[]             = {0x00};
static const uint8_t POF_V[]             = {0x00};

/* ----------------------------- low-level ----------------------------- */

/* Assert the selected chip-select line(s). CS is active low. */
static inline void cs_select(uint8_t sel)
{
    if (sel & CS0_SEL) gpio_put(EPD_PIN_CS_M, 0);
    if (sel & CS1_SEL) gpio_put(EPD_PIN_CS_S, 0);
}

/* Deassert both chip-selects (idle). */
static inline void cs_deselect(void)
{
    gpio_put(EPD_PIN_CS_M, 1);
    gpio_put(EPD_PIN_CS_S, 1);
}

/*
 * Bit-bang bytes, MSB first, SPI mode 0 (clock idle low, data sampled on the
 * rising edge). The panel never drives data back, so there is no read phase.
 * If colours ever come out corrupted, slow this down by adding a short
 * busy_wait between the edges.
 */
/* Transmit on the hardware SPI block (mode 0, MSB first; configured in
 * epd_gpio_init). The panel is write-only, so we never read back. */
static void spi_tx(const uint8_t *data, size_t len)
{
    spi_write_blocking(EPD_SPI, data, len);
}

/*
 * Send one command (with D/C low) and optional data bytes (with D/C high) to
 * the selected controller(s), inside a single chip-select assertion. Mirrors
 * inky_el133uf1.py _send_command().
 */
static void send_command(uint8_t sel, uint8_t cmd, const uint8_t *data, size_t n)
{
    cs_select(sel);
    gpio_put(EPD_PIN_DC, 0);             /* command */
    sleep_ms(DC_SETUP_MS);
    spi_tx(&cmd, 1);
    if (data != NULL && n > 0) {
        gpio_put(EPD_PIN_DC, 1);         /* data */
        spi_tx(data, n);
    }
    cs_deselect();
}

/*
 * Wait for a refresh to finish.
 *
 * BUSY polarity here is the Waveshare/EL133 convention, confirmed by scope-like
 * logging on this panel: BUSY is driven LOW while the panel is refreshing and
 * released HIGH (via the pull-up) when done. A full refresh is ~20 s. We settle
 * first so the refresh has actually asserted BUSY, then hold until it returns
 * high, capped so a stuck line cannot hang the firmware.
 */
static void wait_refresh_done(void)
{
    sleep_ms(2000);                         /* let the refresh assert BUSY low */
    uint32_t ms = 2000;
    while (gpio_get(EPD_PIN_BUSY) == 0 && ms < 60000) {   /* 0 = busy */
        sleep_ms(100);
        ms += 100;
    }
    printf("epd: refresh complete after %ums\n", (unsigned)ms);
}

/* Single reset pulse: RST low 30 ms, high 30 ms, then settle. RST is active
 * low. Matches inky_el133uf1.py setup(). */
static void hw_reset(void)
{
    gpio_put(EPD_PIN_RST, 0); sleep_ms(30);
    gpio_put(EPD_PIN_RST, 1); sleep_ms(30);
    sleep_ms(300);
}

/* ------------------------------ public ------------------------------- */

void epd_gpio_init(void)
{
    /* SCLK + MOSI are driven by the hardware SPI block. */
    spi_init(EPD_SPI, EPD_SPI_HZ);
    spi_set_format(EPD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);  /* mode 0 */
    gpio_set_function(EPD_PIN_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(EPD_PIN_MOSI, GPIO_FUNC_SPI);

    /* CS, D/C, and reset are plain GPIO outputs we drive by hand. */
    const uint outputs[] = {
        EPD_PIN_CS_M, EPD_PIN_CS_S, EPD_PIN_DC, EPD_PIN_RST,
    };
    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        gpio_init(outputs[i]);
        gpio_set_dir(outputs[i], GPIO_OUT);
    }
    gpio_init(EPD_PIN_BUSY);
    gpio_set_dir(EPD_PIN_BUSY, GPIO_IN);
    gpio_pull_up(EPD_PIN_BUSY);   /* matches the Inky host config; idle line reads high */

    /* Idle levels: both controllers deselected, reset released. */
    gpio_put(EPD_PIN_DC,   0);
    gpio_put(EPD_PIN_RST,  1);
    cs_deselect();
}

/*
 * Init / power configuration, ported one-to-one from inky_el133uf1.py setup().
 * Order and CS targeting are exact: ANTM and the power/boost block address only
 * the master controller (CS0); POFS is programmed separately for each
 * controller with different values; the rest are broadcast to both.
 */
void epd_panel_init(void)
{
    hw_reset();

    send_command(CS0_SEL, ANTM,  ANTM_V,  sizeof(ANTM_V));

    send_command(CS_BOTH, CMD66, CMD66_V, sizeof(CMD66_V));
    send_command(CS_BOTH, PSR,   PSR_V,   sizeof(PSR_V));
    send_command(CS0_SEL, DCDC,  DCDC_V,  sizeof(DCDC_V));
    send_command(CS_BOTH, PLL,   PLL_V,   sizeof(PLL_V));
    send_command(CS_BOTH, CDI,   CDI_V,   sizeof(CDI_V));
    send_command(CS_BOTH, TCON,  TCON_V,  sizeof(TCON_V));

    send_command(CS0_SEL, POFS,  POFS_CS0_V, sizeof(POFS_CS0_V));
    send_command(CS1_SEL, POFS,  POFS_CS1_V, sizeof(POFS_CS1_V));

    send_command(CS_BOTH, AGID,  AGID_V,  sizeof(AGID_V));
    send_command(CS_BOTH, PWS,   PWS_V,   sizeof(PWS_V));
    send_command(CS_BOTH, CCSET, CCSET_V, sizeof(CCSET_V));
    send_command(CS_BOTH, TRES,  TRES_V,  sizeof(TRES_V));

    send_command(CS0_SEL, CMDA4, CMDA4_V, sizeof(CMDA4_V));
    send_command(CS0_SEL, PWR,   PWR_V,   sizeof(PWR_V));
    send_command(CS0_SEL, EN_BUF, EN_BUF_V, sizeof(EN_BUF_V));
    send_command(CS0_SEL, BTST_P, BTST_P_V, sizeof(BTST_P_V));
    send_command(CS0_SEL, BOOST_VDDP_EN, BOOST_VDDP_EN_V, sizeof(BOOST_VDDP_EN_V));
    send_command(CS0_SEL, BTST_N, BTST_N_V, sizeof(BTST_N_V));
    send_command(CS0_SEL, BUCK_BOOST_VDDN, BUCK_BOOST_VDDN_V, sizeof(BUCK_BOOST_VDDN_V));
    send_command(CS0_SEL, TFT_VCOM_POWER,  TFT_VCOM_POWER_V,  sizeof(TFT_VCOM_POWER_V));

    printf("epd: init complete\n");
}

void epd_write_frame(epd_fill_row_fn fill)
{
    uint8_t row[EPD_HALF_ROW_BYTES];

    /* One DTM (data transfer) command per controller: CS0 gets the left half,
     * CS1 the right half, each streamed row by row inside one CS assertion. */
    const uint8_t sel[2]        = { CS0_SEL,        CS1_SEL        };
    const enum epd_side side[2] = { EPD_SIDE_LEFT,  EPD_SIDE_RIGHT };
    const uint8_t dtm = DTM;

    for (int s = 0; s < 2; s++) {
        cs_select(sel[s]);
        gpio_put(EPD_PIN_DC, 0);
        sleep_ms(DC_SETUP_MS);
        spi_tx(&dtm, 1);
        gpio_put(EPD_PIN_DC, 1);
        for (int y = 0; y < EPD_HEIGHT; y++) {
            fill(row, side[s], y);
            spi_tx(row, EPD_HALF_ROW_BYTES);
        }
        cs_deselect();
    }
}

/* PON -> wait -> DRF -> wait -> POF. The "actually paint the pixels" step.
 * Matches inky_el133uf1.py _update(). */
void epd_panel_refresh(void)
{
    send_command(CS_BOTH, PON, NULL, 0);
    sleep_ms(300);                         /* power-on settle */

    send_command(CS_BOTH, DRF, DRF_V, sizeof(DRF_V));
    wait_refresh_done();                   /* the actual paint, ~20 s */

    send_command(CS_BOTH, POF, POF_V, sizeof(POF_V));
    sleep_ms(300);                         /* power-off settle */
    printf("epd: refresh done\n");
}
