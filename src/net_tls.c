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

struct altcp_tls_config *net_tls_config(void)
{
    if (s_tried) return s_conf;
    s_tried = true;

    mbedtls_platform_set_time(tesserae_time);
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    net_tls_log_heap("tls-init");

#ifdef TESSERAE_TLS_DEBUG
    /* lwIP's glue installs the debug CALLBACK but never raises mbedTLS's global
     * threshold, which defaults to 0 and suppresses every message before the
     * callback is ever reached. 3 gives state transitions and errors with
     * file:line, which is what identifies the failing step. */
    mbedtls_debug_set_threshold(3);
#endif

    /* PEM: the length must INCLUDE the NUL terminator, hence sizeof. Passing
     * strlen() here is the classic way to get a silent parse failure. */
    s_conf = altcp_tls_create_config_client((const u8_t *)k_ca_roots_pem,
                                            sizeof k_ca_roots_pem);
    if (s_conf == NULL) {
        printf("tls: could not create the client config (root store or memory)\n");
        return NULL;
    }
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

