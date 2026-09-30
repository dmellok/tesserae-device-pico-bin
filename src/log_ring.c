/*
 * log_ring.c: redaction and the byte ring. See log_ring.h.
 */
#include "log_ring.h"

#include <string.h>

/* ---------- redaction ---------- */

/* True for a byte that can still be part of a URL. A URL in a log line is
 * delimited by whitespace or by the quoting the log itself added, so stopping
 * at any of these is both safe and enough. */
static bool url_byte(char c)
{
    return c != '\0' && c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
           c != '"'  && c != '\'' && c != ')' && c != ',';
}

/* Append literal text, clipped to the remaining capacity. Returns the new
 * write offset. Clipping rather than failing keeps a redaction from ever
 * turning into a buffer overrun on a pathologically long line. */
static size_t put(char *dst, size_t cap, size_t n, const char *s, size_t len)
{
    if (n >= cap) return n;
    size_t room = cap - n;
    if (len > room) len = room;
    memcpy(dst + n, s, len);
    return n + len;
}

size_t log_redact(const char *src, size_t len, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) return 0;

    size_t n = 0;
    for (size_t i = 0; i < len && n < cap; ) {

        /* ANSI CSI escape: ESC '[' ... final byte in @..~. The Pico's own
         * printf output has none of these, but a vendored library or a future
         * colourised build could, and a half-stripped escape in an uploaded
         * log is worse to read than none at all. */
        if (src[i] == 0x1B && i + 1 < len && src[i + 1] == '[') {
            size_t j = i + 2;
            while (j < len && !(src[j] >= '@' && src[j] <= '~')) j++;
            i = (j < len) ? j + 1 : len;
            continue;
        }

        /* "Bearer <token>" -> "Bearer <redacted>". Checked before the URL rule
         * because a bearer token never contains "://" and this is the cheaper
         * test of the two. */
        if (src[i] == 'B' && len - i >= 7 && memcmp(src + i, "Bearer ", 7) == 0) {
            n = put(dst, cap, n, "Bearer <redacted>", 17);
            size_t j = i + 7;
            while (j < len && url_byte(src[j])) j++;   /* skip the token */
            i = j;
            continue;
        }

        /* http:// or https:// -> keep the URL, drop any query string. The path
         * is useful for diagnosis (which endpoint, which device id); the query
         * is where Tesserae puts frame tokens, so it never leaves the device. */
        if (src[i] == 'h' &&
            ((len - i >= 7 && memcmp(src + i, "http://",  7) == 0) ||
             (len - i >= 8 && memcmp(src + i, "https://", 8) == 0))) {
            size_t j = i;
            size_t q = 0;                       /* offset of '?', 0 = none */
            while (j < len && url_byte(src[j])) {
                if (src[j] == '?' && q == 0) q = j;
                j++;
            }
            size_t keep = (q != 0) ? q : j;     /* URL up to the query */
            n = put(dst, cap, n, src + i, keep - i);
            if (q != 0) n = put(dst, cap, n, "?<redacted>", 11);
            i = j;
            continue;
        }

        /* The batch is uploaded as text/plain, so anything that is not text
         * is replaced rather than passed through. A NUL in particular would
         * truncate every strlen-based consumer between here and the server. */
        char c = src[i++];
        if (c != '\n' && c != '\t' && (unsigned char)c < 0x20) c = '.';
        dst[n++] = c;
    }
    return n;
}

/* ---------- ring ---------- */

void log_ring_init(log_ring_t *r, uint8_t *buf, size_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->head = 0;
    r->wrapped = false;
}

void log_ring_reset(log_ring_t *r)
{
    r->head = 0;
    r->wrapped = false;
}

void log_ring_write(log_ring_t *r, const void *data, size_t len)
{
    if (r->buf == NULL || r->cap == 0 || len == 0) return;
    const uint8_t *p = (const uint8_t *)data;

    /* A write bigger than the ring can only keep its tail, so skip straight to
     * the last cap bytes rather than churning the whole ring to the same end. */
    if (len >= r->cap) {
        p += len - r->cap;
        len = r->cap;
        r->wrapped = true;
        r->head = 0;
    }

    size_t first = r->cap - r->head;
    if (first > len) first = len;
    memcpy(r->buf + r->head, p, first);

    size_t rest = len - first;
    if (rest > 0) memcpy(r->buf, p + first, rest);

    size_t new_head = r->head + len;
    if (new_head >= r->cap) {
        r->wrapped = true;
        new_head -= r->cap;
    }
    r->head = new_head;
}

size_t log_ring_len(const log_ring_t *r)
{
    return r->wrapped ? r->cap : r->head;
}

size_t log_ring_copy(const log_ring_t *r, void *dst, size_t cap)
{
    if (r->buf == NULL || dst == NULL || cap == 0) return 0;

    size_t have = log_ring_len(r);
    /* Oldest byte: right after head once wrapped, index 0 otherwise. */
    size_t start = r->wrapped ? r->head : 0;

    /* Too small: drop from the OLDEST end. The newest bytes are the ones that
     * explain what the device was doing when it stopped. */
    if (have > cap) {
        start = (start + (have - cap)) % r->cap;
        have = cap;
    }

    uint8_t *out = (uint8_t *)dst;
    size_t first = r->cap - start;
    if (first > have) first = have;
    memcpy(out, r->buf + start, first);
    if (have > first) memcpy(out + first, r->buf, have - first);
    return have;
}
