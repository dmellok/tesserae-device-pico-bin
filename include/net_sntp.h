/*
 * net_sntp.h: one-shot SNTP time sync on lwIP's SNTP app. WiFi must be up.
 *
 * On success the epoch is handed to the sleep manager's wall-clock (used for
 * heartbeat timestamps and sleep_until reporting). Time is best-effort: a
 * failure here never blocks the fetch/paint/sleep cycle.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Poll the configured NTP server until it answers or the timeout elapses.
 * Returns true if the clock was set. */
bool sntp_sync(uint32_t timeout_ms);
