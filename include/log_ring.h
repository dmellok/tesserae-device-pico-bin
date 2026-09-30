/*
 * log_ring.h: the pure half of log capture, kept free of hardware so it can be
 * reasoned about (and tested on a host) on its own.
 *
 * Two pieces:
 *
 *   - log_redact(), which rewrites one log line into a safe form. This runs on
 *     EVERY line before it reaches any buffer, because the alternative is a
 *     device that quietly ships its own bearer token to the server in a
 *     diagnostic upload. See the redaction section of the Protocol v1 spec.
 *
 *   - a byte ring that overwrites its oldest bytes. Log volume is unbounded and
 *     the buffer is not, so something has to give; losing the START of a long
 *     wake is far better than losing the end, since the end is where the paint
 *     and the sleep entry are.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Copy one line from src into dst, stripping ANSI colour codes and redacting
 * secrets. Returns the number of bytes written (never more than cap).
 *
 * Redactions, per the spec:
 *   - the query string of any http:// or https:// URL becomes "?<redacted>",
 *     since Tesserae puts frame tokens there;
 *   - "Bearer <token>" becomes "Bearer <redacted>".
 *
 * Note that a redaction can make a line LONGER ("?a=1" is shorter than
 * "?<redacted>"), so this cannot be done safely in place and the caller must
 * treat cap as a hard limit rather than an expectation. */
size_t log_redact(const char *src, size_t len, char *dst, size_t cap);

/* A byte ring over caller-supplied storage. Nothing here allocates, so the
 * caller decides whether the bytes live in SRAM, PSRAM or a flash shadow. */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   head;     /* where the next byte goes */
    bool     wrapped;  /* true once the ring has overwritten at least once */
} log_ring_t;

void   log_ring_init(log_ring_t *r, uint8_t *buf, size_t cap);

/* Append len bytes, discarding the oldest to make room. A write larger than
 * the whole ring keeps only its tail, which is the useful end. */
void   log_ring_write(log_ring_t *r, const void *data, size_t len);

/* How many bytes the ring currently holds. */
size_t log_ring_len(const log_ring_t *r);

/* Copy the contents oldest-first into dst. When dst is too small, the OLDEST
 * bytes are dropped so the newest always survive. Returns bytes written. */
size_t log_ring_copy(const log_ring_t *r, void *dst, size_t cap);

/* Drop everything. */
void   log_ring_reset(log_ring_t *r);
