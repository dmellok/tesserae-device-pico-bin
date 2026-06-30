#include "battery.h"

#include "hardware/adc.h"   /* also pulls ADC_BASE_PIN from platform_defs.h */
#include "pico/stdlib.h"    /* board header -> PICO_VSYS_PIN */

#ifndef PICO_VSYS_PIN
#error "PICO_VSYS_PIN not defined (wrong board header?)"
#endif

/* Per-board ADC offset correction, in the VSYS (post-/3) millivolt domain.
 * Left at 0 (uncalibrated): battery_mv then reports the honest VSYS rail.
 *
 * Calibrating is only meaningful with the device AT REST ON THE BATTERY (USB
 * unplugged, no charge current) -- while USB feeds VSYS the reading is the
 * ~4.7 V system rail (5 V minus the onboard Schottky drop), not the cell's
 * state of charge. To calibrate: run on battery, compare battery_raw /
 * battery_mv against a multimeter on VSYS at two charge levels, and set this
 * (and a gain term if needed). The E9 input-leakage erratum is handled
 * separately by adc_gpio_init disabling the digital input on GP43. */
#define BATTERY_CAL_MV   0

static uint16_t s_raw = 0;
static uint32_t s_mv  = 0;
static int      s_pct = 0;

void battery_sample(void)
{
    /* GP43 = ADC input (PICO_VSYS_PIN - ADC_BASE_PIN = 43 - 40 = 3) on the
     * RP2350B. The board feeds VSYS/3 into it. Must run before the radio claims
     * the shared pin; see battery.h. */
    adc_init();
    adc_gpio_init(PICO_VSYS_PIN);
    adc_select_input(PICO_VSYS_PIN - ADC_BASE_PIN);

    adc_read();                          /* discard first sample after mux switch */
    uint32_t sum = 0;
    const int N = 8;
    for (int i = 0; i < N; i++) sum += adc_read();
    uint32_t raw = sum / (uint32_t)N;
    s_raw = (uint16_t)raw;

    /* 12-bit conversion, 3.3 V reference, on-board /3 divider on VSYS, plus the
     * per-board ADC offset correction (signed; clamp at 0). */
    int32_t mv = (int32_t)(raw * 3u * 3300u / 4096u) + BATTERY_CAL_MV;
    if (mv < 0) mv = 0;
    s_mv = (uint32_t)mv;

    /* Rough single-cell LiPo state-of-charge: clamp-linear 3.30 V (0%) to
     * 4.20 V (100%). LiPo discharge is nonlinear so this is only an estimate;
     * the server can recalibrate from battery_mv. */
    if      (s_mv >= 4200) s_pct = 100;
    else if (s_mv <= 3300) s_pct = 0;
    else                   s_pct = (int)((s_mv - 3300u) * 100u / (4200u - 3300u));
}

uint16_t battery_raw(void) { return s_raw; }
uint32_t battery_mv(void)  { return s_mv; }
int      battery_pct(void) { return s_pct; }
