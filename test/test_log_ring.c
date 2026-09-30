/*
 * test_log_ring.c: host tests for redaction and the ring (log_ring.c).
 *
 * Runs on the build machine, not the device, because the code under test is
 * deliberately free of hardware. Worth having because a redaction bug does not
 * announce itself: it ships the device token to the server inside a diagnostic
 * upload and everything still looks like it worked.
 *
 *   cc -std=c11 -I include -o /tmp/t test/test_log_ring.c src/log_ring.c && /tmp/t
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "log_ring.h"

static int fails = 0;
static void eq(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("FAIL %s\n  got  '%s'\n  want '%s'\n", what, got, want); fails++;
    } else printf("ok   %s\n", what);
}
static const char *red(const char *in, char *buf, size_t cap) {
    size_t n = log_redact(in, strlen(in), buf, cap - 1);
    buf[n] = '\0'; return buf;
}

int main(void) {
    char b[1024];

    eq("plain line untouched", red("wifi: connected, ip=192.168.50.66", b, sizeof b),
       "wifi: connected, ip=192.168.50.66");

    eq("bearer redacted",
       red("Authorization: Bearer abc123SECRETtoken", b, sizeof b),
       "Authorization: Bearer <redacted>");

    eq("url query redacted",
       red("http: GET https://h/api/v1/device/x/frame?token=SECRET&v=2", b, sizeof b),
       "http: GET https://h/api/v1/device/x/frame?<redacted>");

    eq("url without query kept whole",
       red("http: POST http://192.168.50.125:8765/api/v1/device/pico/status", b, sizeof b),
       "http: POST http://192.168.50.125:8765/api/v1/device/pico/status");

    eq("trailing text after url survives",
       red("GET https://h/p?q=1 took 9s", b, sizeof b),
       "GET https://h/p?<redacted> took 9s");

    eq("ansi stripped", red("\x1b[0;32mI (5) tag: hi\x1b[0m", b, sizeof b),
       "I (5) tag: hi");

    eq("NUL becomes a dot", (log_redact("a\0b", 3, b, sizeof b), b[3]='\0', b), "a.b");

    /* Redaction must never write past cap, even when it lengthens the line. */
    char small[12];
    size_t n = log_redact("https://h/p?aaaaaaaaaaaaaaaa", 28, small, sizeof small);
    printf("%s  bounded write: %zu <= %zu\n", n <= sizeof small ? "ok  " : "FAIL", n, sizeof small);
    if (n > sizeof small) fails++;

    /* Ring keeps the NEWEST bytes on overflow. */
    uint8_t rb[8]; log_ring_t r; log_ring_init(&r, rb, sizeof rb);
    log_ring_write(&r, "0123456789", 10);
    char outb[16]; size_t m = log_ring_copy(&r, outb, sizeof outb); outb[m] = '\0';
    eq("ring keeps newest on oversize write", outb, "23456789");

    log_ring_init(&r, rb, sizeof rb);
    log_ring_write(&r, "abc", 3); log_ring_write(&r, "defgh", 5);
    m = log_ring_copy(&r, outb, sizeof outb); outb[m] = '\0';
    eq("ring wraps correctly", outb, "abcdefgh");

    log_ring_write(&r, "ij", 2);
    m = log_ring_copy(&r, outb, sizeof outb); outb[m] = '\0';
    eq("ring drops oldest after wrap", outb, "cdefghij");

    m = log_ring_copy(&r, outb, 4); outb[m] = '\0';
    eq("small dst keeps newest", outb, "ghij");

    printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
    return fails != 0;
}
