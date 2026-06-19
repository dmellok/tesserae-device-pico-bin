/*
 * sleepmgr.c: RP2350 POWMAN deep sleep + wall-clock. See sleepmgr.h.
 *
 * Wall-clock: the POWMAN timer (1 kHz, lposc-sourced) holds epoch milliseconds
 * once SNTP sets it; it lives in the always-on domain so it keeps counting
 * through deep sleep. We treat a timer value past 2017 as "time is set".
 *
 * Persistence: POWMAN scratch words survive low-power mode. Writes to the
 * POWMAN block carry a password in the top 16 bits, so we only trust the low
 * 16 bits of a scratch word and always write it with the password attached --
 * correct whether or not scratch itself enforces the password.
 */
#include "sleepmgr.h"

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/powman.h"
#include "hardware/structs/powman.h"
#include "hardware/watchdog.h"

#define WAKE_MAGIC      0xB007u                 /* low-16 marker: a deep sleep has happened */
#define EPOCH_VALID_MS  1500000000000ull        /* ~2017-07-14; below this, time is unset   */

static wake_reason_t s_reason;
static uint32_t      s_boot;
static bool          s_latched;

static inline void scratch_put16(int i, uint32_t v)
{
    powman_hw->scratch[i] = POWMAN_PASSWORD_BITS | (v & 0xFFFFu);
}
static inline uint32_t scratch_get16(int i)
{
    return powman_hw->scratch[i] & 0xFFFFu;
}

void sleep_timer_init(void)
{
    if (!powman_timer_is_running()) {
        powman_timer_set_1khz_tick_source_lposc();
        powman_timer_start();
    }
}

static void latch(void)
{
    if (s_latched) return;
    if (scratch_get16(0) == WAKE_MAGIC) {
        s_reason = WAKE_TIMER;
        s_boot   = scratch_get16(1) + 1;
    } else {
        s_reason = WAKE_COLD;
        s_boot   = 1;
    }
    scratch_put16(1, s_boot);
    s_latched = true;
}

wake_reason_t sleep_wake_reason(void) { latch(); return s_reason; }
uint32_t      sleep_boot_count(void)  { latch(); return s_boot;   }

void sleep_set_epoch(uint32_t sec)
{
    sleep_timer_init();
    powman_timer_set_ms((uint64_t)sec * 1000ull);
}

uint32_t sleep_epoch_now(void)
{
    uint64_t ms = powman_timer_get_ms();
    return (ms > EPOCH_VALID_MS) ? (uint32_t)(ms / 1000ull) : 0;
}

void sleep_deep_ms(uint64_t ms)
{
    sleep_timer_init();
    scratch_put16(0, WAKE_MAGIC);   /* next boot will read this as a timer wake */

    /* Deepest state: every domain off (the switched core powers down, so
     * set_power_state never returns). On wake, power everything back up. */
    powman_power_state off = POWMAN_POWER_STATE_NONE;
    powman_power_state on  = POWMAN_POWER_STATE_NONE;
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SRAM_BANK0);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SRAM_BANK1);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_XIP_CACHE);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SWITCHED_CORE);

    if (!powman_configure_wakeup_state(off, on)) {
        printf("sleep: invalid wakeup state; rebooting instead\n");
        watchdog_reboot(0, 0, 0);
        return;
    }
    powman_set_debug_power_request_ignored(true);   /* allow sleep with a debugger attached */

    uint64_t now = powman_timer_get_ms();
    powman_enable_alarm_wakeup_at_ms(now + ms);

    powman_set_power_state(off);

    /* Should be unreachable; reboot as a fallback if power-down was refused. */
    printf("sleep: power-down refused; rebooting\n");
    watchdog_reboot(0, 0, 0);
}
