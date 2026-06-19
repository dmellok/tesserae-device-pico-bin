/*
 * psram.c: RP2350 APS6404 PSRAM init. Ported from MicroPython rp2_psram.c
 * (psram_detect + psram_init), which derives from the Raspberry Pi reference in
 * pico-sdk-rp2350 issue #12. See psram.h.
 *
 * Both functions run from RAM (__no_inline_not_in_flash_func): while QMI direct
 * mode is enabled the flash on CS0 is inaccessible, so any code/literal fetched
 * from XIP would hang. Interrupts are disabled for the same reason.
 */
#include "psram.h"

#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "pico/platform.h"

static size_t __no_inline_not_in_flash_func(psram_detect)(void)
{
    size_t psram_size = 0;

    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}

    /* Exit QPI in case we were already inited, so the 0x9F ID read is in SPI. */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | 0xf5;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    /* Read ID (0x9F): byte 5 = KGD, byte 6 = EID (density). */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (size_t i = 0; i < 7; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9f : 0xff;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {}
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
        if (i == 5)      kgd = qmi_hw->direct_rx;
        else if (i == 6) eid = qmi_hw->direct_rx;
        else             (void)qmi_hw->direct_rx;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd == 0x5D) {
        psram_size = 1024 * 1024;
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2)  psram_size *= 8;   /* 8 MiB (APS6404) */
        else if (size_id == 0)            psram_size *= 2;
        else if (size_id == 1)            psram_size *= 4;
    }
    return psram_size;
}

size_t __no_inline_not_in_flash_func(psram_init)(uint cs_pin)
{
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    uint32_t intr_stash = save_and_disable_interrupts();

    size_t psram_size = psram_detect();
    if (psram_size == 0) {
        restore_interrupts(intr_stash);
        return 0;
    }

    /* Read clock before entering direct mode (flash is then inaccessible). */
    const int max_psram_freq = 133000000;
    const int clock_hz = clock_get_hz(clk_sys);

    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    /* Enter QPI mode (0x35). */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) rxdelay += 1;

    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int max_select   = (125 * 1000000) / clock_period_fs;            /* <= 8us  */
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs
                             - (divisor + 1) / 2;                          /* >= 18ns */

    qmi_hw->m[1].timing =
        1 << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        max_select   << QMI_M1_TIMING_MAX_SELECT_LSB |
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
        rxdelay      << QMI_M1_TIMING_RXDELAY_LSB |
        divisor      << QMI_M1_TIMING_CLKDIV_LSB;

    /* QPI fast read 0xEB, all-quad, 6 dummy cycles. */
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB |
        6 << QMI_M0_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;

    /* QPI write 0x38, all-quad, no dummy. */
    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;

    qmi_hw->direct_csr = 0;                                  /* back to memory-mapped XIP */
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);   /* allow writes to CS1 */

    restore_interrupts(intr_stash);
    return psram_size;
}

bool psram_test(size_t size_bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_XIP_BASE;
    size_t words = size_bytes / sizeof(uint32_t);
    for (size_t i = 0; i < words; i++) p[i] = (uint32_t)(i * 0x9E3779B1u);
    __dmb();
    for (size_t i = 0; i < words; i++) {
        if (p[i] != (uint32_t)(i * 0x9E3779B1u)) return false;
    }
    return true;
}
