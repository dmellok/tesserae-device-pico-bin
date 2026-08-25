/*
 * frame_codec.h: optional DEFLATE decompression for incoming frames.
 *
 * A panel-native frame is enormously compressible -- a real 1600x1200 4bpp
 * Spectra 6 render measures 960,000 bytes raw and 6,855 deflated, 0.7% -- for
 * the obvious reason that an e-ink dashboard is flat colour and sharp text over
 * a six-value palette. That ratio is a property of the format, not of any
 * particular image.
 *
 * It matters far beyond bandwidth. The cloud relay could not fetch a full-size
 * frame at all: Cloudflare ramps its TLS record size partway through a large
 * body and overruns the client's record buffer (see the README's "Known
 * issues"). A ~7 KB frame is a single small request that never reaches the
 * ramp, so compression does not merely speed the transfer up -- it is what
 * makes the relay usable on this board.
 *
 * DETECTION IS BY CONTENT, NOT BY CONTRACT. The device already knows its exact
 * frame size, so a payload of exactly that length is raw, and anything else is
 * examined for a zlib or gzip header. That deliberately avoids inventing a
 * format field the server does not yet send: this code is inert until something
 * starts compressing, works the moment it does, and cannot fall out of step
 * with the other end. The relay's plaintext X-Tesserae-Format header is a
 * reasonable thing to log, but nothing here depends on it.
 *
 * Inflation is only ever applied to bytes that already passed AES-GCM
 * authentication (relay_crypto.h), so a malformed stream cannot come from
 * anyone without the frame key.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if `p` starts with a zlib or gzip header.
 *
 * Both are cheap and unambiguous: gzip is the fixed magic 1f 8b, and zlib is a
 * CMF/FLG pair whose low nibble is 8 (deflate) and whose 16-bit big-endian
 * value is a multiple of 31. A raw 4bpp frame beginning with those bytes by
 * coincidence is possible in principle, which is why callers check the exact
 * length first and only consult this when the length does not match. */
bool frame_looks_compressed(const uint8_t *p, size_t len);

/* Inflate a zlib or gzip stream into `dst`.
 *
 * Returns true only when the stream decoded cleanly, its checksum verified,
 * and it produced exactly `expect_len` bytes -- the frame size the panel
 * demands. A short or long result is a mismatched render rather than something
 * to paint, so it is refused rather than padded.
 *
 * `dst` MUST have expect_len + 1 bytes writable. The extra byte is a guard:
 * uzlib checks its output limit at the bottom of its loop, so detecting an
 * over-long stream requires somewhere safe for that byte to land. Callers here
 * decompress into PSRAM with megabytes to spare, so this costs nothing. */
bool frame_inflate(const uint8_t *src, size_t src_len,
                   uint8_t *dst, size_t expect_len);

#ifdef TESSERAE_FRAME_SELFTEST
/* Inflate a known real frame into `scratch` and verify the result against a
 * checksum computed on the host. Proves the decoder on THIS target, including
 * the PSRAM read-after-write that back-references depend on. Needs
 * FRAME_SELFTEST_RAW_LEN + 1 bytes. */
bool frame_codec_selftest(uint8_t *scratch, size_t scratch_len);
#endif
