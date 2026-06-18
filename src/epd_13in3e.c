/*
 * epd_13in3e.c: Inky Impression 13.3" (EL133UF1 / Spectra 6) driver.
 *
 * Opcodes, argument blobs, and init/refresh sequencing originate from
 * Waveshare's official EL133UF1 / Spectra 6 demo (waveshareteam/
 * ESP32-S3-ePaper-13.3E6), derived from the EL133UF1 datasheet. We follow
 * tesserae-device-esp32-bin/src/epd_driver.c, which adapted that demo to
 * ESP-IDF; only the transport changed:
 *
 *   ESP-IDF                         ->  Pico SDK / this file
 *   spi_device_polling_transmit()   ->  bit-banged spi_tx() on GP35/GP36
 *   gpio_set_level()                ->  gpio_put()
 *   gpio_get_level()                ->  gpio_get()
 *   vTaskDelay(pdMS_TO_TICKS(n))    ->  sleep_ms(n)
 *   ESP_LOGx()                      ->  printf() over USB stdio
 *
 * Why bit-bang: the adapter wires the panel's clock/data to GP35/GP36, which
 * are SPI0 TX / SPI0 RX on the RP2350B. No SPI peripheral routes a clock onto
 * GP35, so the hardware SPI block cannot be used here. We shift bits out by
 * hand. The panel is write-only and BUSY is a separate GPIO, so a transmit
 * only SPI is sufficient. See epd_config.h for the full reasoning.
 */
#include "epd_13in3e.h"
#include "epd_config.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* --- panel command opcodes (EL133UF1; names match the ESP32 port) --- */
#define PSR             0x00   /* panel setting                            */
#define PWR             0x01   /* power setting                            */
#define POF             0x02   /* power off                                */
#define PON             0x04   /* power on                                 */
#define BTST_N          0x05   /* booster soft-start, negative            */
#define BTST_P          0x06   /* booster soft-start, positive            */
#define DTM             0x10   /* data transfer (frame data)               */
#define DRF             0x12   /* display refresh (paints the loaded frame)*/
#define CDI             0x50   /* VCOM and data interval                   */
#define TCON            0x60   /* gate/source timing                       */
#define TRES            0x61   /* resolution setting                       */
#define AN_TM           0x74   /* analogue timing (master-only)            */
#define AGID            0x86   /* gate/source independent driving         */
#define BUCK_BOOST_VDDN 0xB0
#define TFT_VCOM_POWER  0xB1
#define EN_BUF          0xB6
#define BOOST_VDDP_EN   0xB7
#define CCSET           0xE0
#define PWS             0xE3   /* power saving                             */
#define CMD66           0xF0

/* --- canned init parameter blobs (copied verbatim; do NOT edit) --- */
static const uint8_t AN_TM_V[]           = {0xC0, 0x1C, 0x1C, 0xCC, 0xCC, 0xCC, 0x15, 0x15, 0x55};
static const uint8_t CMD66_V[]           = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t PSR_V[]             = {0xDF, 0x69};
static const uint8_t CDI_V[]             = {0xF7};
static const uint8_t TCON_V[]            = {0x03, 0x03};
static const uint8_t AGID_V[]            = {0x10};
static const uint8_t PWS_V[]             = {0x22};
static const uint8_t CCSET_V[]           = {0x01};
static const uint8_t TRES_V[]            = {0x04, 0xB0, 0x03, 0x20};   /* 1200 x 800-per-half */
static const uint8_t PWR_V[]             = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t EN_BUF_V[]          = {0x07};
static const uint8_t BTST_P_V[]          = {0xE8, 0x28};
static const uint8_t BOOST_VDDP_EN_V[]   = {0x01};
static const uint8_t BTST_N_V[]          = {0xE8, 0x28};
static const uint8_t BUCK_BOOST_VDDN_V[] = {0x01};
static const uint8_t TFT_VCOM_POWER_V[]  = {0x02};
static const uint8_t DRF_V[]             = {0x00};
static const uint8_t POF_V[]             = {0x00};

/* ----------------------------- low-level ----------------------------- */

/* Drive both chip-selects to the same level (1 = idle/deselected). */
static inline void cs_both(int level)
{
    gpio_put(EPD_PIN_CS_M, level);
    gpio_put(EPD_PIN_CS_S, level);
}

/*
 * Bit-bang one byte, MSB first, SPI mode 0 (clock idle low, data sampled on
 * the rising edge). Back-to-back gpio_put() on the RP2350B at 150 MHz yields a
 * clock comfortably within the panel's tolerance; if a build ever shows
 * corrupted/garbage colour, slow this down by adding a short busy_wait between
 * the edges. The panel never drives data back, so there is no read phase.
 */
static void spi_tx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int bit = 7; bit >= 0; bit--) {
            gpio_put(EPD_PIN_MOSI, (b >> bit) & 1u);
            gpio_put(EPD_PIN_SCLK, 1);   /* rising edge: slave samples MOSI */
            gpio_put(EPD_PIN_SCLK, 0);
        }
    }
}

/* A command byte is sent with D/C low. CS is managed by the caller so that a
 * command and its data ride inside one chip-select assertion. */
static void send_cmd(uint8_t cmd)
{
    gpio_put(EPD_PIN_DC, 0);
    spi_tx(&cmd, 1);
}

static void send_data(const uint8_t *buf, size_t len)
{
    gpio_put(EPD_PIN_DC, 1);
    spi_tx(buf, len);
}

static void cmd_with_data(uint8_t cmd, const uint8_t *buf, size_t len)
{
    send_cmd(cmd);
    send_data(buf, len);
}

/*
 * Block until the panel reports idle.
 *
 * BUSY polarity here follows the Inky Impression, not the Waveshare panel: on
 * the Inky board BUSY reads HIGH while the panel is working and LOW once it is
 * ready (pimoroni/inky inky_el133uf1.py), the opposite of the Waveshare ESP32
 * demo this driver otherwise tracks. The Inky PCB inverts the line. The input
 * also carries a pull-up (see epd_gpio_init), so a disconnected BUSY reads HIGH
 * (treated as "busy") and trips the timeout rather than floating.
 *
 * A short settle lets the panel assert BUSY after the triggering command, so
 * we do not race past a refresh that has not started pulling BUSY high yet.
 * timeout_ms caps the wait so a stuck or disconnected line cannot hang the
 * firmware; pass a value above the operation's real duration (a full refresh
 * is ~25-35 s).
 */
static void wait_idle(uint32_t timeout_ms)
{
    sleep_ms(50);                          /* let BUSY assert after the command */

    const uint32_t poll_ms = 10;
    uint32_t elapsed_ms = 0;
    while (gpio_get(EPD_PIN_BUSY) == 1) {   /* 1 = busy */
        sleep_ms(poll_ms);
        elapsed_ms += poll_ms;
        if (elapsed_ms >= timeout_ms) {
            printf("epd: BUSY still high after %ums, continuing (check wiring/power)\n",
                   (unsigned)(elapsed_ms + 50));
            return;
        }
    }
}

/* Reset pulse train the EL133UF1 wants to latch: three high/low cycles
 * (30 ms each level) then settle high. Matches the ESP32 port's hw_reset(). */
static void hw_reset(void)
{
    for (int i = 0; i < 3; i++) {
        gpio_put(EPD_PIN_RST, 1); sleep_ms(30);
        gpio_put(EPD_PIN_RST, 0); sleep_ms(30);
    }
    gpio_put(EPD_PIN_RST, 1); sleep_ms(30);
}

/* ------------------------------ public ------------------------------- */

void epd_gpio_init(void)
{
    const uint outputs[] = {
        EPD_PIN_SCLK, EPD_PIN_MOSI, EPD_PIN_CS_M,
        EPD_PIN_CS_S, EPD_PIN_DC,   EPD_PIN_RST,
    };
    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        gpio_init(outputs[i]);
        gpio_set_dir(outputs[i], GPIO_OUT);
    }
    gpio_init(EPD_PIN_BUSY);
    gpio_set_dir(EPD_PIN_BUSY, GPIO_IN);
    gpio_pull_up(EPD_PIN_BUSY);   /* matches the Inky host config; idle line reads high */

    /* Idle levels: clock low, both controllers deselected, reset released. */
    gpio_put(EPD_PIN_SCLK, 0);
    gpio_put(EPD_PIN_DC,   0);
    gpio_put(EPD_PIN_RST,  1);
    cs_both(1);
}

/*
 * Init sequence, from the Waveshare EL133UF1 demo by way of the ESP32 port's
 * epd_init() (one-to-one). The CS
 * targeting is deliberate and preserved exactly: a handful of commands address
 * only the master controller (CS_M), which holds the chip-shared registers,
 * while the rest are broadcast to both controllers (CS_M and CS_S together).
 */
void epd_panel_init(void)
{
    hw_reset();
    wait_idle(5000);   /* panel settles quickly after reset */

    /* Master-only: analogue timing. */
    gpio_put(EPD_PIN_CS_M, 0);
    cmd_with_data(AN_TM, AN_TM_V, sizeof(AN_TM_V));
    cs_both(1);

    /* Broadcast to both controllers. */
    #define BOTH(cmd, val) do {            \
        cs_both(0);                         \
        cmd_with_data((cmd), (val), sizeof(val)); \
        cs_both(1);                         \
    } while (0)

    BOTH(CMD66, CMD66_V);
    BOTH(PSR,   PSR_V);
    BOTH(CDI,   CDI_V);
    BOTH(TCON,  TCON_V);
    BOTH(AGID,  AGID_V);
    BOTH(PWS,   PWS_V);
    BOTH(CCSET, CCSET_V);
    BOTH(TRES,  TRES_V);

    #undef BOTH

    /* Master-only: power and booster programming. */
    #define MASTER(cmd, val) do {           \
        gpio_put(EPD_PIN_CS_M, 0);          \
        cmd_with_data((cmd), (val), sizeof(val)); \
        cs_both(1);                         \
    } while (0)

    MASTER(PWR,             PWR_V);
    MASTER(EN_BUF,          EN_BUF_V);
    MASTER(BTST_P,          BTST_P_V);
    MASTER(BOOST_VDDP_EN,   BOOST_VDDP_EN_V);
    MASTER(BTST_N,          BTST_N_V);
    MASTER(BUCK_BOOST_VDDN, BUCK_BOOST_VDDN_V);
    MASTER(TFT_VCOM_POWER,  TFT_VCOM_POWER_V);

    #undef MASTER

    printf("epd: init complete\n");
}

void epd_write_frame(epd_fill_row_fn fill)
{
    uint8_t row[EPD_HALF_ROW_BYTES];

    /* One DTM (data transfer) command per controller, with the controller's
     * 480000-byte half streamed row by row inside a single CS assertion. */
    const int cs_pins[2] = { EPD_PIN_CS_M, EPD_PIN_CS_S };
    const enum epd_side sides[2] = { EPD_SIDE_LEFT, EPD_SIDE_RIGHT };

    for (int s = 0; s < 2; s++) {
        gpio_put(cs_pins[s], 0);          /* select this controller only */
        send_cmd(DTM);
        for (int y = 0; y < EPD_HEIGHT; y++) {
            fill(row, sides[s], y);
            send_data(row, EPD_HALF_ROW_BYTES);
        }
        cs_both(1);
    }
}

/* PON -> wait -> DRF -> wait -> POF. The "actually paint the pixels" step. */
void epd_panel_refresh(void)
{
    cs_both(0); send_cmd(PON); cs_both(1);
    wait_idle(5000);                       /* power-on settles quickly */

    sleep_ms(50);
    cs_both(0); cmd_with_data(DRF, DRF_V, sizeof(DRF_V)); cs_both(1);
    wait_idle(45000);                      /* the actual paint: ~25-35 s */

    sleep_ms(50);
    cs_both(0); cmd_with_data(POF, POF_V, sizeof(POF_V)); cs_both(1);
    wait_idle(5000);                       /* power-off settles quickly */
    printf("epd: refresh done\n");
}
