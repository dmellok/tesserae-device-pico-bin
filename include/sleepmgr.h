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

/* Start the always-on POWMAN timer and apply the 1 kHz LPOSC tick source
 * (using the calibrated LPOSC frequency if sleep_calibrate_lposc() has run). */
void sleep_timer_init(void);

/* Measure the LPOSC frequency against the accurate system timer and store it
 * so the POWMAN tick divider produces a true 1 kHz (the LPOSC is untrimmed and
 * runs well off its 32.768 kHz nominal, ~14% here). One 1-second measurement
 * per power session: it self-skips on timer-wake boots (the calibration is kept
 * in an always-on scratch register), so only a cold boot pays the cost. */
void sleep_calibrate_lposc(void);

/* Why this boot happened. Latches on first call (and records the boot count). */
wake_reason_t sleep_wake_reason(void);

/* Make the NEXT boot report WAKE_COLD.
 *
 * A watchdog_reboot() out of the setup portal (or a factory reset) leaves the
 * deep-sleep wake marker in place, so the reboot would otherwise look like a
 * routine timer wake. It is not: the device has just been reconfigured, nothing
 * on the glass can be trusted, and every across-wake counter should start over.
 * Call this immediately before rebooting. */
void sleep_force_cold_boot(void);

/* Boots since the last cold start (1 on a cold boot). */
uint32_t sleep_boot_count(void);

/* Consecutive-connectivity-failure counter, kept in an always-on scratch word so
 * it survives the deep-sleep reboots between cycles (reset on a cold boot). Used
 * to re-open the setup portal after repeated failures to reach WiFi/the server. */
uint32_t sleep_failcount(void);
void     sleep_failcount_inc(void);
void     sleep_failcount_reset(void);

/* What the panel is currently showing (a splash_state_t, stored as a plain
 * integer so sleepmgr.h does not have to know about splash.h). Kept in an
 * always-on scratch word so it survives the deep sleep between cycles, which is
 * what lets main.c repaint a status screen only on a real STATE CHANGE instead
 * of burning a 20-45 s refresh on every retry. Reset to 0 (SPLASH_NONE) on a
 * cold boot, since a power cycle may well have changed what is on the glass. */
uint32_t sleep_splash_state(void);
void     sleep_set_splash_state(uint32_t state);

/* Consecutive wakes whose cloud-relay device-token requests were answered 401.
 * Lives here for the same reason as the failure counter: "across wakes" means
 * "across deep sleeps", and this must not cost a flash write on what is meant
 * to be a rare path. A cold boot clears it, which errs toward staying paired. */
uint32_t sleep_relay_authfail(void);
void     sleep_relay_authfail_inc(void);
void     sleep_relay_authfail_reset(void);

/* Last panel variant the model EEPROM successfully reported, or 0xFF if none
 * has been seen this power session.
 *
 * The EEPROM is a separate I2C chip from the SPI panel and is known to go quiet
 * after rapid warm reboots (see CLAUDE.md); a clean power cycle revives it. That
 * matters far more than it sounds, because the panel driver -- and therefore the
 * ENTIRE frame path -- is selected from this variant. One unanswered read used
 * to mean the device silently stopped asking the server for images at all, while
 * still posting healthy heartbeats. Remembering the answer here bridges exactly
 * the gap that fails: POWMAN scratch survives the deep sleeps and warm reboots
 * where the EEPROM sulks, and after a true power cycle the EEPROM answers again
 * anyway.
 *
 * Deliberately NOT stored in the flash config: growing config_t moves the CRC
 * offset, which invalidates every saved blob -- including a working cloud-relay
 * pairing. A panel identity is not worth making people re-pair for. */
uint8_t sleep_panel_variant(void);
void    sleep_set_panel_variant(uint8_t variant);

/* Raw POWMAN timer value in milliseconds.
 *
 * Unlike sleep_epoch_now() this never returns 0 and keeps counting across deep
 * sleeps, which is what makes it usable for "has N hours passed" over a span of
 * many wakes. It is NOT strictly monotonic, though: it is the same timer the
 * wall clock lives in, so sleep_set_epoch() makes it jump forward by decades.
 * Callers measuring an interval must therefore treat an implausible delta as a
 * clock correction and re-baseline rather than as elapsed time. */
uint64_t sleep_raw_ms(void);

/* Set the wall-clock from an epoch (seconds). Called by the SNTP layer. */
void sleep_set_epoch(uint32_t sec);

/* Current wall-clock epoch seconds, or 0 if it has not been set this session
 * (survives deep sleep once set, since the POWMAN timer keeps running). */
uint32_t sleep_epoch_now(void);

/* Power-gate the chip and wake via the POWMAN alarm after `ms`. Does not
 * return: the chip resets and main() re-runs on wake. */
void sleep_deep_ms(uint64_t ms);

/* Power-gate the chip with no wake source armed: only a reset (button) or power
 * cycle brings it back. Does not return. Used by the portal idle timeout. */
void sleep_deep_until_reset(void);
