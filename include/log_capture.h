/*
 * log_capture.h: keep the device's own log so the server can ask for it.
 * Implements the firmware half of the device log upload plan, Protocol v1.
 *
 * Every printf still reaches USB serial exactly as before; this tees a copy,
 * redacted (log_ring.h), into two places:
 *
 *   - a WAKE buffer in SRAM holding this boot, and
 *   - a CARRY region in flash holding the not-yet-uploaded tail of earlier
 *     boots.
 *
 * The flash half is where this port diverges from the ESP32 client, and it is
 * not a stylistic choice. That firmware keeps its cross-sleep ring in RTC
 * memory, which survives deep sleep. The RP2350 has no equivalent: POWMAN deep
 * sleep power-gates the switched core, so SRAM and PSRAM are both gone on
 * wake, and the always-on domain is eight scratch words of which sleepmgr.c
 * already uses six. Flash is the only writable storage that survives, and it
 * has the side benefit of surviving a power cut too, which RTC memory does not.
 *
 * Carrying the tail across a sleep matters more here than on the ESP32,
 * because main.c drops the radio BEFORE the 20-45 s paint. The paint, the etag
 * save and the sleep entry therefore all log with no way to send anything, and
 * those lines are exactly the ones worth reading when a panel stops updating.
 * They can only ever be delivered on the following wake.
 *
 * Cost: one flash erase plus program per wake, spread over LOG_FLASH_SECTORS
 * sectors so no single sector takes every cycle.
 *
 * Nothing is uploaded unless the server asks. A /status response carrying
 * "logs": {"upload": true} makes the caller run log_capture_upload() while the
 * radio is still up; the status body advertises the capability every beat.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bytes of this boot's log kept in SRAM. A quiet wake logs well under 1 KB; a
 * frame download logs a progress line per 8 KB, so a 960 KB frame alone is
 * about 120 lines. 8 KB holds a busy wake whole and still costs little against
 * the 512 KB part. */
#define LOG_WAKE_BYTES   8192

/* Bytes carried across a sleep in flash. This is the number advertised to the
 * server as logs.ring_bytes, and 3072 matches the value the spec fixed for the
 * ESP32 client so the two report the same contract. */
#define LOG_CARRY_BYTES  3072

/* Set up the ring, write the boot header line and install the stdio tee.
 * Call as early in main() as stdio allows: lines printed before this are not
 * captured, only printed. Safe to call when flash holds nothing yet. */
void log_capture_init(void);

/* Capacity of the cross-sleep region, advertised as logs.ring_bytes. */
uint32_t log_capture_ring_bytes(void);

/* POST everything not yet uploaded (the flash carry from earlier boots, then
 * this boot so far) as text/plain to <server>/api/v1/device/<id>/log.
 *
 * On any 2xx those bytes are marked delivered and are never sent again. On
 * anything else they stay eligible, and there is no retry this wake: a panel
 * that cannot reach the server has better things to do with its radio time.
 * Returns true on a 2xx. Requires a server URL, so relay-only panels never
 * upload, which is what the spec says for Protocol v1. */
bool log_capture_upload(void);

/* Write the still-unsent tail to flash so the next wake can deliver it.
 *
 * Call once, as late in the cycle as possible and always after the paint,
 * since capturing the paint is the entire point. Does nothing when there is
 * nothing new to keep, so an idle wake costs no flash wear. */
void log_capture_persist(void);
