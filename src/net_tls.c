/*
 * net_tls.c: TLS client configuration. See net_tls.h.
 */
#include "net_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>

#include "pico/stdlib.h"    /* time_us_64 */
#include "lwip/altcp.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"

#include "ca_roots.h"
#include "sleepmgr.h"

static struct altcp_tls_config *s_conf;
static bool s_tried;          /* one init attempt per boot, success or not */

/* ---- heap instrumentation ------------------------------------------------
 *
 * mbedTLS reports an out-of-memory as MBEDTLS_ERR_MPI_ALLOC_FAILED (-0x0010)
 * from wherever the bignum layer happened to be, which says nothing about how
 * much was wanted or what was already gone. Since the whole TLS path lives or
 * dies on the heap, route mbedTLS's allocator through a wrapper that reports
 * the exact failing size, and print the heap picture alongside it. */

extern char __end__;        /* start of the heap (end of .bss) */
extern char __HeapLimit;    /* ceiling sbrk may grow to */
extern char __StackBottom;  /* lowest address the stack may reach */
extern char __StackTop;     /* initial SP */

/* ---- stack high-water measurement ----------------------------------------
 *
 * Stack exhaustion does not fail cleanly on this target: it corrupts whatever
 * sits below the stack and the damage surfaces somewhere unrelated. That makes
 * "is the stack big enough" a question worth ANSWERING rather than assuming,
 * which is how a 2 KB default went unnoticed. Paint the unused region with a
 * pattern, then find the deepest word that got overwritten. */
#define STACK_PAINT 0xC5C5C5C5u

void net_tls_paint_stack(void)
{
    /* Leave a margin above the current frame so this call does not paint over
     * its own locals or the return address it needs. */
    volatile uint32_t here;
    /* Arithmetic in uintptr_t, not on the uint32_t* itself: offsetting a
     * pointer derived from a single local lets GCC apply array-bounds
     * reasoning to it and warn, even though walking the stack is the whole
     * point. The 256-byte margin keeps this function's own frame intact. */
    uintptr_t limit = ((uintptr_t)&here & ~(uintptr_t)3) - 256u;
    for (uint32_t *p = (uint32_t *)&__StackBottom; (uintptr_t)p < limit; p++)
        *p = STACK_PAINT;
}

void net_tls_log_stack(const char *when)
{
    const uint32_t *bottom = (const uint32_t *)&__StackBottom;
    const uint32_t *top    = (const uint32_t *)&__StackTop;
    const uint32_t *p      = bottom;
    while (p < top && *p == STACK_PAINT) p++;

    size_t total = (size_t)((const char *)top - (const char *)bottom);
    size_t used  = (size_t)((const char *)top - (const char *)p);
    printf("stack[%s]: %u of %u bytes used, %u free%s\n", when,
           (unsigned)used, (unsigned)total, (unsigned)(total - used),
           (p == bottom) ? "  *** EXHAUSTED ***" : "");
}

void net_tls_log_heap(const char *when)
{
    struct mallinfo mi = mallinfo();
    char *brk = (char *)sbrk(0);
    printf("heap[%s]: total=%u brk_used=%u unclaimed=%u  arena=%u in_use=%u free=%u\n",
           when,
           (unsigned)(&__HeapLimit - &__end__),
           (unsigned)(brk - &__end__),
           (unsigned)(&__HeapLimit - brk),
           (unsigned)mi.arena, (unsigned)mi.uordblks, (unsigned)mi.fordblks);
}

static void *tls_calloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (p == NULL) {
        printf("tls: CALLOC FAILED for %u x %u = %u bytes\n",
               (unsigned)n, (unsigned)sz, (unsigned)(n * sz));
        net_tls_log_heap("calloc-fail");
    }
    return p;
}

static void tls_free(void *p) { free(p); }

/* ---- wall clock ----------------------------------------------------------
 *
 * mbedTLS needs a time source to check certificate validity dates. This board
 * has no RTC, but it does have the POWMAN timer, which runs in the always-on
 * domain and therefore keeps the wall clock across deep sleeps once anything
 * has set it (the HTTP Date header of a REST response, or SNTP). Zero means
 * "never set this power session", which newlib's time() would otherwise report
 * as 1970 -- and 1970 makes every certificate in existence look
 * not-yet-valid. */
static mbedtls_time_t tesserae_time(mbedtls_time_t *t)
{
    mbedtls_time_t now = (mbedtls_time_t)sleep_epoch_now();
    if (t != NULL) *t = now;
    return now;
}

/* mbedTLS also wants a MONOTONIC millisecond counter (MBEDTLS_PLATFORM_MS_TIME_
 * ALT). Upstream derives it from POSIX clock_gettime, which does not exist
 * here. The Pico's 64-bit microsecond timer is a better source than the wall
 * clock in any case: it never jumps, whereas sleep_epoch_now() moves whenever a
 * server corrects the time -- and a timeout computed across such a jump either
 * fires instantly or never. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(time_us_64() / 1000ull);
}

/* ---- verification --------------------------------------------------------
 *
 * Chain and hostname verification are always enforced. Certificate DATES are
 * the one exception, and only while the clock is unset.
 *
 * The alternative -- failing the handshake when the clock is unknown -- sounds
 * stricter but is worse in practice on a relay-only panel. Such a device has no
 * plaintext HTTP call to learn the time from (the relay is HTTPS-only, so the
 * Date header is behind the very handshake we are trying to complete), which
 * leaves SNTP; and UDP/123 is blocked on a great many guest, hotspot and
 * corporate networks. A cold boot on one of those would then be permanently
 * unable to talk to the relay, with the only symptom being a blank panel. That
 * is a self-inflicted outage.
 *
 * What is actually given up is narrow: an attacker who holds a REVOKED or
 * EXPIRED certificate for the exact hostname, plus the network position to use
 * it, and who catches the device before its clock is set. And even then the
 * dashboard itself stays sealed -- frames are AES-256-GCM under a key derived
 * at pairing that the relay never sees, so a TLS compromise reads a device
 * token and a status heartbeat, not pixels.
 *
 * Once any wake has set the clock (which is the steady state, since it survives
 * deep sleep), dates are checked normally. */
static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx;

    /* Reaching this callback at all is the key diagnostic when a handshake
     * fails: it means we got as far as the server's certificate chain, so the
     * ciphersuite and curve negotiation succeeded and any failure is a TRUST
     * problem (a CA we do not carry, a name mismatch). A handshake that dies
     * without ever calling this failed earlier -- no shared ciphersuite, no
     * shared curve -- which has an entirely different fix. Printing the subject
     * also confirms which chain the relay is actually serving today, without
     * needing openssl on the far end. */
    char subject[128] = "(none)";
    if (crt != NULL)
        mbedtls_x509_dn_gets(subject, sizeof subject, &crt->subject);
    printf("tls: verify depth %d flags 0x%08x: %s\n",
           depth, (unsigned)*flags, subject);

    if (sleep_epoch_now() != 0) return 0;    /* clock is good: no leniency */

    uint32_t stale = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE |
                     MBEDTLS_X509_BADCRL_EXPIRED  | MBEDTLS_X509_BADCRL_FUTURE;
    if (*flags & stale) {
        printf("tls: clock unset; ignoring certificate date flags at depth %d\n",
               depth);
        *flags &= ~stale;
    }
    return 0;
}

#ifdef TESSERAE_TLS_DEBUG
#include "tls_selftest_cert.h"

/* Parse the captured chain with no radio and no sockets involved.
 *
 * Isolates "does this target's mbedTLS build parse this chain" from every
 * networking variable. The bytes are known-good: they were recovered from the
 * device's own level-4 trace and are byte-identical to what openssl sees, and a
 * host build of this same config parses and verifies them cleanly. */
void net_tls_selftest(void)
{
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    net_tls_log_heap("selftest");

    for (int i = 0; i < 3; i++) {
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        int rc = mbedtls_x509_crt_parse_der(&crt, k_selftest_certs[i],
                                            k_selftest_cert_lens[i]);
        if (rc == 0) {
            char subject[128] = "?";
            mbedtls_x509_dn_gets(subject, sizeof subject, &crt.subject);
            printf("selftest: cert%d (%u B) OK  %s\n", i,
                   (unsigned)k_selftest_cert_lens[i], subject);
        } else {
            char buf[128];
            mbedtls_strerror(rc, buf, sizeof buf);
            printf("selftest: cert%d (%u B) FAILED rc=%d (-0x%04X) %s\n", i,
                   (unsigned)k_selftest_cert_lens[i], rc, (unsigned)(-rc), buf);
        }
        mbedtls_x509_crt_free(&crt);
    }

    /* Now the shape the HANDSHAKE uses: all three appended into ONE chain, in
     * order, exactly as ssl_parse_certificate() does. This is the only
     * structural difference left between the parse that works here and the one
     * that fails during the handshake -- appending walks the existing list and
     * allocates a new link each time, where the per-cert test above always
     * starts from a freshly initialised context. */
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    for (int i = 0; i < 3; i++) {
        int rc = mbedtls_x509_crt_parse_der(&chain, k_selftest_certs[i],
                                            k_selftest_cert_lens[i]);
        if (rc != 0) {
            char buf[128];
            mbedtls_strerror(rc, buf, sizeof buf);
            printf("selftest: CHAIN append %d FAILED rc=%d (-0x%04X) %s\n",
                   i, rc, (unsigned)(-rc), buf);
            break;
        }
        printf("selftest: chain append %d OK\n", i);
    }

    /* Reproduce the handshake's exact memory layout: mbedTLS parses at
     * ssl->in_msg + 10 (4-byte handshake header, 3-byte cert-list length,
     * 3-byte cert length), which is 2 mod 4 -- UNALIGNED. Every parse above
     * used a compiler-aligned array, so this is the last untested difference
     * between the call that works and the call that fails. */
    for (int off = 0; off <= 10; off += 2) {
        static uint8_t staged[1024];
        if ((size_t)off + k_selftest_cert_lens[0] > sizeof staged) break;
        memcpy(staged + off, k_selftest_certs[0], k_selftest_cert_lens[0]);
        mbedtls_x509_crt one;
        mbedtls_x509_crt_init(&one);
        int r = mbedtls_x509_crt_parse_der(&one, staged + off,
                                           k_selftest_cert_lens[0]);
        printf("selftest: offset %2d (%d mod 4) -> %s (rc=-0x%04X)\n",
               off, off % 4, r == 0 ? "OK" : "FAILED", (unsigned)(-r));
        mbedtls_x509_crt_free(&one);
    }

    /* And the verification the handshake would do next, against our roots. */
    mbedtls_x509_crt roots;
    mbedtls_x509_crt_init(&roots);
    int rc = mbedtls_x509_crt_parse(&roots, (const unsigned char *)k_ca_roots_pem,
                                    sizeof k_ca_roots_pem);
    if (rc != 0) {
        printf("selftest: roots parse FAILED rc=-0x%04X\n", (unsigned)(-rc));
    } else {
        uint32_t flags = 0;
        rc = mbedtls_x509_crt_verify(&chain, &roots, NULL, "relay.tesserae.ink",
                                     &flags, NULL, NULL);
        printf("selftest: verify vs roots rc=%d (-0x%04X) flags=0x%08x\n",
               rc, (unsigned)(-rc), (unsigned)flags);
        if (flags) {
            char info[256];
            mbedtls_x509_crt_verify_info(info, sizeof info, "selftest:   ", flags);
            fputs(info, stdout);
        }
    }
    mbedtls_x509_crt_free(&roots);
    mbedtls_x509_crt_free(&chain);

    net_tls_log_heap("selftest-done");
}
#endif

struct altcp_tls_config *net_tls_config(void)
{
    if (s_tried) return s_conf;
    s_tried = true;

    mbedtls_platform_set_time(tesserae_time);
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    net_tls_log_heap("tls-init");
#ifdef TESSERAE_TLS_DEBUG
    /* Run the offline parse here rather than from main(): stdio-USB drops
     * everything printed before the host raises DTR, so a boot-time call
     * is simply never seen. By the time TLS is first needed the port is
     * long since up. */
    net_tls_selftest();
#endif

#ifdef TESSERAE_TLS_DEBUG
    /* lwIP's glue installs the debug CALLBACK but never raises mbedTLS's global
     * threshold, which defaults to 0 and suppresses every message before the
     * callback is ever reached. 3 gives state transitions and errors with
     * file:line, which is what identifies the failing step; 4 adds a hexdump of
     * every record as it comes off the wire, which is the only way to compare
     * what the device received against what the server actually sent. */
    mbedtls_debug_set_threshold(TESSERAE_TLS_DEBUG >= 2 ? 4 : 3);
#endif

    /* PEM: the length must INCLUDE the NUL terminator, hence sizeof. Passing
     * strlen() here is the classic way to get a silent parse failure. */
    s_conf = altcp_tls_create_config_client((const u8_t *)k_ca_roots_pem,
                                            sizeof k_ca_roots_pem);
    if (s_conf == NULL) {
        printf("tls: could not create the client config (root store or memory)\n");
        return NULL;
    }
    /* Tell the server our records are small (RFC 6066 max_fragment_length).
     *
     * Without this, a 4 KB input buffer is simply non-compliant: a TLS 1.2 peer
     * may send 16 KB records unasked, and Cloudflare does exactly that partway
     * through a large body. With it, a server that honours the extension is
     * obliged to stay inside our buffer.
     *
     * lwIP declares struct altcp_tls_config only inside altcp_tls_mbedtls.c and
     * offers no setter, but mbedtls_ssl_config is its FIRST member, so the
     * handle doubles as a pointer to the config. That is a layout dependency on
     * a vendored file, which is why it is spelled out here rather than hidden
     * in a cast: if lwIP ever reorders that struct this call corrupts the
     * config instead of failing to compile. The failure mode if the extension
     * is simply not honoured is the one we already have. */
    mbedtls_ssl_conf_max_frag_len((mbedtls_ssl_config *)s_conf,
                                  MBEDTLS_SSL_MAX_FRAG_LEN_4096);
    printf("tls: client config ready (%u bytes of built-in roots)\n",
           (unsigned)sizeof k_ca_roots_pem);
    return s_conf;
}

bool net_tls_attach(struct altcp_pcb *pcb, const char *hostname)
{
    if (pcb == NULL || hostname == NULL || hostname[0] == '\0') return false;

    mbedtls_ssl_context *ssl = altcp_tls_context(pcb);
    if (ssl == NULL) {
        printf("tls: no ssl context on the wrapped pcb\n");
        return false;
    }

    /* This single call does double duty: it sends SNI (without which a
     * CDN-fronted host serves a default certificate that will not match) and it
     * is the name mbedTLS checks the CN/SAN against. altcp_tls never calls it,
     * so skipping it means verifying that the certificate is validly signed by
     * a real CA -- for some other domain entirely. */
    int rc = mbedtls_ssl_set_hostname(ssl, hostname);
    if (rc != 0) {
        printf("tls: set_hostname('%s') failed: -0x%04x\n", hostname, -rc);
        return false;
    }
    mbedtls_ssl_set_verify(ssl, verify_cb, NULL);
    return true;
}

void net_tls_deinit(void)
{
    if (s_conf != NULL) {
        altcp_tls_free_config(s_conf);
        s_conf = NULL;
    }
    s_tried = false;
}

/* Turn an mbedTLS return code into something a field log can act on. Exported
 * for net_httpc.c, which is where handshake failures actually surface. */
void net_tls_log_error(const char *what, int rc)
{
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof buf);
    printf("tls: %s failed: -0x%04x (%s)\n", what, -rc, buf);
}

