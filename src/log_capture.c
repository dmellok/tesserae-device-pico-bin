/*
 * log_capture.c: the stdio tee, the flash carry and the upload. See
 * log_capture.h for why the carry lives in flash on this board.
 */
#include "log_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"          /* stdio_set_driver_enabled */
#include "pico/stdio/driver.h"   /* stdio_driver_t is opaque without this */
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"   /* XIP_BASE */

#include "log_ring.h"
#include "config.h"
#include "net_rest.h"
#include "sleepmgr.h"
#include "fw_version.h"

/* Where the carry lives. Deliberately high in the 16 MB part, far above the
 * program (well under 1 MB) and the config sector at 1 MB.
 *
 * The distance is not padding for its own sake. Any future OTA support needs
 * two multi-megabyte image slots, and a slot boundary would naturally fall
 * exactly where a lower offset would sit. Putting the log region at 12 MB
 * means the OTA flash map can be decided later without having to move it and
 * lose whatever it held. */
#define LOG_FLASH_OFFSET   (12u * 1024 * 1024)

/* Sectors rotated round-robin. One erase per wake lands on a different sector
 * each time, so a 15 minute cadence costs each sector about 12 erases a day
 * rather than 96, which against roughly 100k endurance is decades either way
 * but costs nothing to spread. */
#define LOG_FLASH_SECTORS  8

#define LOG_REC_MAGIC      0x474F4C54u   /* "TLOG" little-endian */

/* One flash sector holds this header followed by the bytes. */
typedef struct {
    uint32_t magic;
    uint32_t seq;      /* monotonic; highest valid wins on load */
    uint32_t len;      /* payload bytes that follow */
    uint32_t crc;      /* over magic..len plus the payload */
} log_rec_t;

_Static_assert(sizeof(log_rec_t) + LOG_CARRY_BYTES <= FLASH_SECTOR_SIZE,
               "log carry no longer fits in one flash sector");

/* The longest single line kept whole. Longer lines are split across redaction
 * calls, which can only cost a redaction straddling the boundary, never
 * safety: the worst case is that an unsplit URL is written verbatim. */
#define LOG_LINE_MAX  512

static uint8_t    s_wake_buf[LOG_WAKE_BYTES];
static log_ring_t s_wake;

static uint8_t  s_carry[LOG_CARRY_BYTES];  /* unsent bytes from earlier boots */
static uint32_t s_carry_len;
static uint32_t s_carry_seq;               /* seq of the record we loaded */

/* Total bytes ever handed to the wake ring, and the value that total had when
 * an upload was last accepted. The difference is what still owes delivery,
 * which is what persist() writes. Counting totals rather than ring offsets
 * keeps this correct across a wrap. */
static uint64_t s_wake_total;
static uint64_t s_wake_sent;

static bool s_inited;

/* Line assembly for the tee. stdio hands us arbitrary chunks, not lines, but
 * redaction has to see a whole URL or token to recognise it. */
static char   s_line[LOG_LINE_MAX];
static size_t s_line_len;

/* ---------- the tee ---------- */

static void flush_line(void)
{
    if (s_line_len == 0) return;
    char red[LOG_LINE_MAX + 64];   /* a redaction can lengthen a line */
    size_t n = log_redact(s_line, s_line_len, red, sizeof red);
    log_ring_write(&s_wake, red, n);
    s_wake_total += n;
    s_line_len = 0;
}

/* Called by stdio for every byte the firmware prints. Must not print, must not
 * block and must not touch flash: it runs inside printf, including from printf
 * calls made while interrupts are disabled. */
static void tee_out_chars(const char *buf, int len)
{
    if (!s_inited || buf == NULL || len <= 0) return;
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' || s_line_len == sizeof s_line) {
            if (c == '\n' && s_line_len < sizeof s_line) s_line[s_line_len++] = '\n';
            flush_line();
            if (c != '\n') { s_line[0] = c; s_line_len = 1; }
        } else {
            s_line[s_line_len++] = c;
        }
    }
}

static void tee_out_flush(void) { }

/* next is filled in by stdio_set_driver_enabled; the rest stays NULL because
 * this driver only ever observes output. */
static stdio_driver_t s_tee = {
    .out_chars = tee_out_chars,
    .out_flush = tee_out_flush,
    .in_chars  = NULL,
};

/* ---------- flash carry ---------- */

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static const log_rec_t *sector_at(int i)
{
    return (const log_rec_t *)(XIP_BASE + LOG_FLASH_OFFSET +
                               (uint32_t)i * FLASH_SECTOR_SIZE);
}

static bool rec_valid(const log_rec_t *r)
{
    if (r->magic != LOG_REC_MAGIC) return false;
    if (r->len > LOG_CARRY_BYTES) return false;
    uint32_t want = crc32(r, offsetof(log_rec_t, crc));
    want = crc32((const uint8_t *)r + sizeof *r, r->len) ^ want;
    return r->crc == want;
}

/* Load the newest valid record into s_carry. Erased flash reads 0xFF, so a
 * device that has never written one simply finds nothing and starts clean. */
static void carry_load(void)
{
    const log_rec_t *best = NULL;
    for (int i = 0; i < LOG_FLASH_SECTORS; i++) {
        const log_rec_t *r = sector_at(i);
        if (!rec_valid(r)) continue;
        /* Unsigned wrap-safe comparison, so a seq that rolls over after 4
         * billion wakes still picks the newer record. */
        if (best == NULL || (int32_t)(r->seq - best->seq) > 0) best = r;
    }
    if (best == NULL) { s_carry_len = 0; s_carry_seq = 0; return; }

    s_carry_len = best->len;
    s_carry_seq = best->seq;
    memcpy(s_carry, (const uint8_t *)best + sizeof *best, s_carry_len);
}

static void carry_store(const uint8_t *data, uint32_t len)
{
    if (len > LOG_CARRY_BYTES) {        /* keep the newest */
        data += len - LOG_CARRY_BYTES;
        len = LOG_CARRY_BYTES;
    }

    static uint8_t page[FLASH_SECTOR_SIZE];
    memset(page, 0xFF, sizeof page);

    log_rec_t *r = (log_rec_t *)page;
    r->magic = LOG_REC_MAGIC;
    r->seq   = s_carry_seq + 1;
    r->len   = len;
    memcpy(page + sizeof *r, data, len);
    r->crc = crc32(r, offsetof(log_rec_t, crc)) ^ crc32(page + sizeof *r, len);

    uint32_t off = LOG_FLASH_OFFSET +
                   (uint32_t)(r->seq % LOG_FLASH_SECTORS) * FLASH_SECTOR_SIZE;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, page, sizeof page);
    restore_interrupts(ints);
}

/* ---------- batch assembly ---------- */

/* Bytes of this boot that have not been delivered, newest-first-preserving.
 * Returns how many were copied into dst. */
static size_t unsent_wake(uint8_t *dst, size_t cap)
{
    uint64_t owed64 = s_wake_total - s_wake_sent;
    size_t   have   = log_ring_len(&s_wake);
    /* The ring may have overwritten some of what is owed; it can only deliver
     * what it still holds. */
    size_t owed = (owed64 > have) ? have : (size_t)owed64;
    if (owed == 0) return 0;

    /* Copy the whole ring, then keep its last `owed` bytes. */
    static uint8_t tmp[LOG_WAKE_BYTES];
    size_t n = log_ring_copy(&s_wake, tmp, sizeof tmp);
    if (owed > n) owed = n;
    if (owed > cap) {          /* keep the newest */
        memcpy(dst, tmp + n - cap, cap);
        return cap;
    }
    memcpy(dst, tmp + n - owed, owed);
    return owed;
}

/* ---------- public ---------- */

void log_capture_init(void)
{
    if (s_inited) return;

    carry_load();
    log_ring_init(&s_wake, s_wake_buf, sizeof s_wake_buf);
    s_wake_total = 0;
    s_wake_sent  = 0;
    s_inited = true;

    /* Install before the header so the header itself is captured. */
    stdio_set_driver_enabled(&s_tee, true);

    /* Per-boot header, exactly the shape Protocol v1 fixes. reset= is omitted
     * rather than guessed: the abnormal reasons the spec names (brownout,
     * panic, watchdog) are not distinguishable here yet, and inventing a value
     * would make the server's timeline wrong rather than merely incomplete. */
    uint32_t epoch = sleep_epoch_now();
    printf("# tesserae-log v1 fw=%s boot=%u wake=%s epoch=%lu\n",
           FW_VERSION, (unsigned)sleep_boot_count(),
           (sleep_wake_reason() == WAKE_TIMER) ? "timer" : "poweron",
           (unsigned long)epoch);
}

uint32_t log_capture_ring_bytes(void) { return LOG_CARRY_BYTES; }

bool log_capture_upload(void)
{
    if (!s_inited) return false;
    if (!config_has_server()) return false;   /* relay-only: not in v1 */

    size_t cap = s_carry_len + LOG_WAKE_BYTES;
    uint8_t *batch = malloc(cap);
    if (batch == NULL) {
        printf("log: no memory for a %u byte batch\n", (unsigned)cap);
        return false;
    }

    size_t n = 0;
    if (s_carry_len > 0) {
        memcpy(batch, s_carry, s_carry_len);
        n = s_carry_len;
    }
    n += unsent_wake(batch + n, cap - n);

    if (n == 0) { free(batch); return true; }   /* nothing owed is success */

    uint64_t mark = s_wake_total;   /* what this batch covers */
    rest_status_t st = rest_post_log((const char *)batch, n, 10000);
    free(batch);

    if (st != REST_OK) {
        printf("log: upload failed (%d); %u bytes stay eligible\n",
               (int)st, (unsigned)n);
        return false;
    }

    printf("log: uploaded %u bytes\n", (unsigned)n);
    s_carry_len  = 0;      /* delivered; never send these again */
    s_wake_sent  = mark;
    return true;
}

void log_capture_persist(void)
{
    if (!s_inited) return;

    static uint8_t out[LOG_CARRY_BYTES];
    size_t n = 0;

    /* Anything the upload did not carry stays owed, oldest first. */
    if (s_carry_len > 0) {
        n = (s_carry_len > sizeof out) ? sizeof out : s_carry_len;
        memcpy(out, s_carry + (s_carry_len - n), n);
    }

    uint8_t tail[LOG_CARRY_BYTES];
    size_t t = unsent_wake(tail, sizeof tail);

    if (t >= sizeof out) {            /* this boot alone fills the record */
        memcpy(out, tail + (t - sizeof out), sizeof out);
        n = sizeof out;
    } else if (t > 0) {
        if (n + t > sizeof out) {     /* drop the oldest carry to make room */
            size_t keep = sizeof out - t;
            memmove(out, out + (n - keep), keep);
            n = keep;
        }
        memcpy(out + n, tail, t);
        n += t;
    }

    /* Nothing new owed: leave flash alone rather than burning an erase to
     * rewrite what is already there. */
    if (n == 0 || (t == 0 && s_carry_len == 0)) return;

    carry_store(out, (uint32_t)n);
}
