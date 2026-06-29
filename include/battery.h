/*
 * battery.h: VSYS / battery-voltage sense.
 *
 * The Pico Plus 2 W (RP2350B) feeds VSYS/3 into ADC input on GP43
 * (PICO_VSYS_PIN). That pin is shared with the RM2 wireless
 * (CYW43_USES_VSYS_PIN), so the read must happen BEFORE the radio is brought
 * up: call battery_sample() once at cycle start (each wake is a full reboot, so
 * the pin is free then and no cyw43 lock is needed). The cached value is then
 * read by the heartbeat builders.
 *
 * Assumes a battery wired to the VSYS pin (so VSYS ~= battery voltage). On USB
 * power VSYS reads ~5 V and the percentage pins at 100.
 */
#pragma once

#include <stdint.h>

/* Sample VSYS and cache mV + percentage. Call once per wake, before wifi. */
void battery_sample(void);

/* Last sampled VSYS in millivolts (0 before the first sample). */
uint32_t battery_mv(void);

/* Rough single-cell LiPo state-of-charge, 0..100 (estimate; see battery.c). */
int battery_pct(void);
