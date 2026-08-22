/*
 * net_tls.h: the TLS client configuration shared by every https:// request.
 *
 * Wraps lwIP's altcp_tls (mbedTLS) with the two things it does not do for you:
 * a trust store (include/ca_roots.h) and hostname verification. Both matter --
 * altcp_tls defaults to MBEDTLS_SSL_VERIFY_REQUIRED but never calls
 * mbedtls_ssl_set_hostname(), so without net_tls_attach() a connection would
 * validate the chain and then happily accept a valid certificate for an
 * entirely different domain.
 *
 * One config is built lazily and reused for the life of the boot: parsing the
 * root store costs a few milliseconds and ~6 KB, and every request in a wake
 * goes to the same host anyway.
 */
#pragma once

#include <stdbool.h>

#include "lwip/altcp_tls.h"

/* The shared client config, or NULL if TLS could not be initialised (a root
 * store that failed to parse, or no memory). Callers treat NULL as "https is
 * unavailable this boot" rather than falling back to plaintext. */
struct altcp_tls_config *net_tls_config(void);

/* Bind `hostname` to a freshly wrapped TLS pcb: sets SNI (mandatory for any
 * CDN-fronted host, which is all of them) and, because the same field drives
 * mbedTLS's CN/SAN check, turns on hostname verification.
 *
 * Also installs the verify callback described in net_tls.c, which relaxes
 * certificate DATE checking -- and only date checking -- while the wall clock
 * is unset. Returns false if the hostname could not be applied, in which case
 * the caller must abort the connection rather than proceed unverified. */
bool net_tls_attach(struct altcp_pcb *pcb, const char *hostname);

/* Release the shared config. Called before the radio goes down for a deep
 * sleep; harmless if TLS was never used. */
void net_tls_deinit(void);

/* Print an mbedTLS return code with its decoded message. Handshake failures
 * surface as bare negative hex otherwise, which is unactionable in a field
 * log. */
void net_tls_log_error(const char *what, int rc);

/* Print the heap picture (total, sbrk high-water, unclaimed, malloc arena).
 * The TLS path is the firmware's only large dynamic consumer, so this is the
 * number that decides whether a handshake can succeed at all. */
void net_tls_log_heap(const char *when);
