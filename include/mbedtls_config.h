/*
 * mbedtls_config.h: mbedTLS feature set for this firmware.
 *
 * The Pico SDK bundles mbedTLS 3.6.x but leaves the configuration to the
 * application (pico_mbedtls_config.h simply includes this file). Everything is
 * opt-in here rather than trimmed down from the default: the default config
 * builds ~everything, and on a device whose whole TLS surface is "one outbound
 * HTTPS client to one known host" that is a lot of flash and attack surface for
 * nothing.
 *
 * What this has to support, and nothing else:
 *
 *   - A TLS 1.2 CLIENT (no server, no DTLS, no PSK, no client certificates).
 *     TLS 1.3 is deliberately off: mbedTLS 3.6 routes 1.3 through PSA crypto,
 *     which roughly doubles the build, and every relay endpoint we talk to
 *     still negotiates 1.2 happily.
 *   - ECDHE key exchange with ECDSA or RSA server certificates. That covers the
 *     Google Trust Services (ECDSA P-256, via WE1/GTS Root R4) chain the hosted
 *     relay presents today, and the RSA chains a self-hosted Worker behind a
 *     different CA might.
 *   - AES-GCM, both for the TLS record layer and -- separately -- for unsealing
 *     relay frames (relay_crypto.c), which is also why HKDF is on.
 *   - X.509 chain verification against a small built-in root store, with SNI
 *     and hostname checking. See net_tls.c for the trust decisions.
 *
 * Entropy comes from the RP2350 hardware RNG through the SDK's
 * pico_mbedtls.c (MBEDTLS_ENTROPY_HARDWARE_ALT); there is no filesystem, so
 * MBEDTLS_NO_PLATFORM_ENTROPY is required or mbedTLS looks for /dev/urandom.
 */
#pragma once

/* ---- platform ---- */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY           /* calloc/free from the SDK's heap */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT      /* pico_mbedtls.c: mbedtls_hardware_poll */

/* Certificate validity dates are checked against the POWMAN wall clock, which
 * net_tls.c installs via mbedtls_platform_set_time(). When that clock has never
 * been set (a cold boot with no time source yet) net_tls.c's verify callback
 * clears the resulting expiry flags rather than failing the handshake -- see
 * the long comment there for why that is the right trade on an RTC-less board. */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT

/* MBEDTLS_HAVE_TIME also pulls in mbedtls_ms_time(), a MONOTONIC millisecond
 * counter used for TLS 1.3 / DTLS timers. Upstream implements it from POSIX
 * clock_gettime and #errors out on anything else, so bare metal must supply it.
 * net_tls.c wires it to the Pico's own microsecond timer -- which is the right
 * source anyway: it is monotonic and always running, unlike the wall clock
 * above, which can be unset or jump when a server corrects it. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* ---- hashes ---- */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C                  /* SHA-384 signatures in the GTS chain */
#define MBEDTLS_HKDF_C                    /* relay frame-key derivation */

/* ---- ciphers ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C

/* ---- public key ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21                 /* RSASSA-PSS: modern RSA chains use it */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

/* ---- RNG ---- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HMAC_DRBG_C               /* required by ECDSA_DETERMINISTIC */

/* ---- X.509 ---- */
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C               /* the built-in roots are PEM */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- TLS ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION           /* SNI: required by any CDN */
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET

/* ON by default upstream, and easy to lose when writing a config from scratch.
 * Without it the handshake takes mbedtls_x509_crt_parse_der_NOCOPY, which keeps
 * pointers into ssl->in_msg instead of copying the chain out. That buffer is
 * the live record-reassembly area: it keeps being written as the handshake
 * continues, so the peer chain can be overwritten underneath the parsed
 * structures. Both paths parse this server's chain correctly in isolation
 * (verified on the host), which is exactly what makes the in-place variant the
 * suspect -- its failure mode needs a live buffer to show up at all. */
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* Record buffers. Inbound must be the full 16 KB: TLS 1.2 has no negotiated way
 * to make a server send smaller records, and a server that sends a max-size one
 * to a client with a short buffer produces a fatal alert, not a retry.
 * Outbound is capped low on purpose -- our largest request is a status POST of
 * a few hundred bytes, and each KB here is a KB of SRAM held for the whole
 * connection. */
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096

/* Error strings cost about 8 KB of flash (of 16 MB) and turn "handshake failed:
 * -0x2700" into something actionable in a field log. Worth it. */
#define MBEDTLS_ERROR_C

/* MBEDTLS_DEBUG_C is what makes the library's own handshake trace possible.
 * Without it, debug.c compiles to an empty object and lwIP's
 * mbedtls_ssl_conf_dbg() hook is never able to say anything -- so a failed
 * handshake reports only its final return code, with no indication of which
 * step produced it. Debug-build only: the trace is chatty enough to alter the
 * timing it is being used to investigate. */
#ifdef TESSERAE_TLS_DEBUG
#define MBEDTLS_DEBUG_C
#endif

/* Do NOT include mbedtls/check_config.h here. In mbedTLS 3.x, build_info.h
 * pulls this file in and only THEN applies its config_adjust_*.h layer, which
 * is what derives MBEDTLS_MD_CAN_SHA256 and friends from the primitives above.
 * Running the checker from inside the config sees the pre-adjustment state and
 * reports a cascade of "defined, but not all prerequisites" for a config that
 * is perfectly valid. build_info.h runs the checker itself, in the right
 * place. */
