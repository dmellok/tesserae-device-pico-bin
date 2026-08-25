/*
 * frame_codec.c: DEFLATE decompression for incoming frames. See frame_codec.h.
 */
#include "frame_codec.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "vendor/uzlib/uzlib.h"

bool frame_looks_compressed(const uint8_t *p, size_t len)
{
    if (p == NULL || len < 2) return false;

    /* gzip: fixed magic. */
    if (p[0] == 0x1f && p[1] == 0x8b) return true;

    /* zlib (RFC 1950): CM must be 8 (deflate), and the CMF/FLG pair read as a
     * big-endian 16-bit value must be a multiple of 31. FDICT set would mean a
     * preset dictionary we do not have, so treat that as not-ours rather than
     * failing later in the middle of a stream. */
    uint8_t cmf = p[0], flg = p[1];
    if ((cmf & 0x0f) != 8) return false;
    if (flg & 0x20) return false;                    /* FDICT */
    return (((unsigned)cmf << 8) | flg) % 31u == 0u;
}

bool frame_inflate(const uint8_t *src, size_t src_len,
                   uint8_t *dst, size_t expect_len)
{
    if (src == NULL || dst == NULL || src_len == 0 || expect_len == 0) return false;

    struct uzlib_uncomp d;
    memset(&d, 0, sizeof d);
    d.source          = src;
    d.source_limit    = src + src_len;
    d.source_read_cb  = NULL;
    d.dest_start      = dst;
    d.dest            = dst;
    /* One byte BEYOND the frame, which the caller guarantees is writable.
     *
     * uzlib tests `dest < dest_limit` at the BOTTOM of its loop, so a call that
     * starts with the buffer already full still executes one iteration and can
     * emit a byte. Sizing the limit at expect_len exactly would therefore make
     * the "did the stream end where it should" check itself a one-byte
     * overrun. The guard byte turns an over-long stream into something we
     * detect and refuse, inside our own buffer. */
    d.dest_limit      = dst + expect_len + 1;

    /* dict = NULL means back-references resolve against the output buffer
     * itself. That is correct here precisely because the whole frame is
     * decompressed into one contiguous buffer -- there is no windowing, so the
     * full 32 KB history is always addressable. */
    uzlib_uncompress_init(&d, NULL, 0);

    /* Both parsers report failure as a NEGATIVE code; success is TINF_OK for
     * gzip but the window-size exponent (0..7) for zlib, so testing against
     * TINF_OK rejects every valid zlib stream. */
    bool gzip = (src[0] == 0x1f && src_len > 1 && src[1] == 0x8b);
    int rc = gzip ? uzlib_gzip_parse_header(&d) : uzlib_zlib_parse_header(&d);
    if (rc < 0) {
        printf("frame: %s header rejected (%d)\n", gzip ? "gzip" : "zlib", rc);
        return false;
    }

    /* uzlib returns TINF_OK when it stopped because the output buffer filled,
     * not only on error, so this has to be driven to TINF_DONE rather than
     * called once. _chksum rather than the plain call because it verifies the
     * stream's own adler32/crc32 on completion: the GCM tag already proved the
     * bytes came from the key holder, so this is not about trust -- it catches
     * a truncated or mis-assembled transfer before it becomes a picture. */
    do {
        rc = uzlib_uncompress_chksum(&d);
    } while (rc == TINF_OK && d.dest < d.dest_limit);

    if (rc != TINF_DONE) {
        printf("frame: inflate failed (%d) after %u bytes\n",
               rc, (unsigned)(size_t)(d.dest - dst));
        return false;
    }

    size_t out = (size_t)(d.dest - dst);
    if (out != expect_len) {
        /* Decoded cleanly but is not this panel's frame. Painting it would put
         * a sheared or half-blank image on the glass, which reads as a hardware
         * fault rather than a mismatch. */
        printf("frame: inflated to %u bytes, panel wants %u; refusing\n",
               (unsigned)out, (unsigned)expect_len);
        return false;
    }
    return true;
}

#ifdef TESSERAE_FRAME_SELFTEST
#include "frame_selftest.h"
#include "vendor/uzlib/uzlib.h"

bool frame_codec_selftest(uint8_t *scratch, size_t scratch_len)
{
    if (scratch == NULL || scratch_len < FRAME_SELFTEST_RAW_LEN + 1) {
        printf("frame-selftest: need %u bytes of scratch, have %u\n",
               (unsigned)FRAME_SELFTEST_RAW_LEN + 1, (unsigned)scratch_len);
        return false;
    }

    /* Poison first, so a decoder that silently under-produces cannot pass by
     * leaving a previously-correct buffer untouched. */
    memset(scratch, 0xA5, FRAME_SELFTEST_RAW_LEN);

    absolute_time_t t0 = get_absolute_time();
    bool ok = frame_inflate(k_frame_selftest_z, sizeof k_frame_selftest_z,
                            scratch, FRAME_SELFTEST_RAW_LEN);
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    if (!ok) { printf("frame-selftest: inflate FAILED\n"); return false; }

    /* Checksum the OUTPUT independently of the stream's own adler32, which
     * uzlib already verified: this is checking that what landed in PSRAM is
     * what the host compressed, not merely that the stream was self-consistent. */
    uint32_t adler = uzlib_adler32(scratch, FRAME_SELFTEST_RAW_LEN, 1);
    bool match = (adler == FRAME_SELFTEST_RAW_ADLER);
    printf("frame-selftest: %u -> %u bytes in %u ms (%u KB/s), adler %08x %s\n",
           (unsigned)sizeof k_frame_selftest_z, (unsigned)FRAME_SELFTEST_RAW_LEN,
           (unsigned)(us / 1000), us ? (unsigned)((uint64_t)FRAME_SELFTEST_RAW_LEN * 1000u / (uint64_t)us) : 0u,
           (unsigned)adler, match ? "OK" : "MISMATCH");
    return match;
}
#endif
