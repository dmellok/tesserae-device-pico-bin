#include "battery.h"

#include "hardware/adc.h"   /* also pulls ADC_BASE_PIN from platform_defs.h */
#include "pico/stdlib.h"    /* board header -> PICO_VSYS_PIN */

#ifndef PICO_VSYS_PIN
#error "PICO_VSYS_PIN not defined (wrong board header?)"
#endif

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

    /* 12-bit conversion, 3.3 V reference, on-board /3 divider on VSYS. */
    s_mv = raw * 3u * 3300u / 4096u;

    /* Rough single-cell LiPo state-of-charge: clamp-linear 3.30 V (0%) to
     * 4.20 V (100%). LiPo discharge is nonlinear so this is only an estimate;
     * the server can recalibrate from battery_mv. */
    if      (s_mv >= 4200) s_pct = 100;
    else if (s_mv <= 3300) s_pct = 0;
    else                   s_pct = (int)((s_mv - 3300u) * 100u / (4200u - 3300u));
}

uint32_t battery_mv(void)  { return s_mv; }
int      battery_pct(void) { return s_pct; }
