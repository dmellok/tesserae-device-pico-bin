/*
 * sleepmgr.h: deep sleep + wall-clock on the RP2350 POWMAN block.
 *
 * sleep_deep_ms() power-gates the chip (switched core off) and arms the POWMAN
 * alarm to wake after a delay. This is real deep sleep, not a timer-reboot
 * loop: the core loses power and the chip resets on wake, so main() re-runs
 * from the top. State that must survive a sleep lives in the always-on POWMAN
 * domain -- the running timer (our wall-clock) and a couple of scratch words
 * (wake magic + boot count).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WAKE_COLD  = 0,   /* power-on / first boot (no prior deep sleep seen) */
    WAKE_TIMER = 1,   /* woke from a POWMAN alarm after sleep_deep_ms()   */
} wake_reason_t;

/* Start the always-on POWMAN timer if it is not already running. Idempotent:
 * after a timer wake the timer is still running and keeps the wall-clock, so
 * this leaves it untouched. */
void sleep_timer_init(void);

/* Why this boot happened. Latches on first call (and records the boot count). */
wake_reason_t sleep_wake_reason(void);

/* Boots since the last cold start (1 on a cold boot). */
uint32_t sleep_boot_count(void);

/* Set the wall-clock from an epoch (seconds). Called by the SNTP layer. */
void sleep_set_epoch(uint32_t sec);

/* Current wall-clock epoch seconds, or 0 if it has not been set this session
 * (survives deep sleep once set, since the POWMAN timer keeps running). */
uint32_t sleep_epoch_now(void);

/* Power-gate the chip and wake via the POWMAN alarm after `ms`. Does not
 * return: the chip resets and main() re-runs on wake. */
void sleep_deep_ms(uint64_t ms);
