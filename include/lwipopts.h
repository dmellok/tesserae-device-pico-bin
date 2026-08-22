/*
 * lwipopts.h: lwIP configuration for the CYW43 WiFi stack.
 *
 * NO_SYS / threadsafe-background mode (no RTOS), matching how the picosdk
 * framework builds pico_cyw43_arch here. Based on the canonical pico-examples
 * "examples_common" lwIP options, extended for the apps this firmware uses
 * (DNS, SNTP, mDNS, MQTT, HTTPD). App source files are added to the build as
 * each feature lands; the options here are harmless until then.
 */
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
/* Headroom for TLS: a 16 KB inbound record is reassembled from many segments,
 * and the handshake bursts several KB outbound in one write. The stock 16000 /
 * 24 works for plain HTTP but starves under altcp_tls, where the symptom is an
 * ERR_MEM mid-handshake rather than anything that names memory. */
#define MEM_SIZE                    32000
#define MEMP_NUM_TCP_SEG            48
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              32
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_MSS                     1460
/* TCP_WND must be at least MBEDTLS_SSL_IN_CONTENT_LEN (16384), or altcp_tls
 * warns "TCP_WND is smaller than the RX decryption buffer, connection RX might
 * stall" -- and it is right: a peer that sends a full-size TLS record cannot
 * fit it inside the advertised window, so mbedTLS waits for a record that the
 * window will not let arrive. 12 MSS = 17520 clears it with margin. */
#define TCP_WND                     (12 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_IGMP                   1   /* needed for mDNS multicast */
#define SO_REUSE                    1

/* mDNS responder: advertise tesserae-pico-XXXX.local while connected. */
#define LWIP_MDNS_RESPONDER         1
#define LWIP_NUM_NETIF_CLIENT_DATA  2   /* mDNS stores per-netif state here   */
#define MDNS_MAX_SERVICES           1
#define MEMP_NUM_UDP_PCB            8   /* DHCP + DNS + SNTP + mDNS + headroom */
/* altcp is the connection abstraction net_httpc.c is written against: the same
 * client code drives plain TCP and TLS, which is what lets an http:// home
 * server and an https:// cloud relay share one implementation. */
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1

/* IPv6, SLAAC only (no DHCPv6): a v6-only or v6-preferred network can host the
 * Tesserae server, and without this the device simply cannot reach it. DNS asks
 * for both families and takes whichever answers, so a dual-stack network keeps
 * behaving exactly as it did. */
#define LWIP_IPV6                   1
#define LWIP_IPV6_AUTOCONFIG        1
#define LWIP_IPV6_DHCP6             0
#define LWIP_ICMP6                  1
#define LWIP_IPV6_MLD               1
#define MEMP_NUM_ND6_QUEUE          4
#define LWIP_DNS_ADDRTYPE_DEFAULT   LWIP_DNS_ADDRTYPE_IPV4_IPV6

/* extra timeouts headroom for the apps (DHCP/DNS/SNTP/MQTT/mDNS timers) */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

/* MQTT output buffer: must hold a full publish (topic + payload + header). The
 * default 256 is too small for the ~210-byte heartbeat JSON at QoS1+retain,
 * which fails with ERR_MEM. 1024 leaves comfortable headroom. */
#define MQTT_OUTPUT_RINGBUF_SIZE    1024

/* SNTP (phase 5): resolve the server by name, and deliver the epoch through an
 * assignment into net_sntp.c's globals (no cross-file prototype to maintain).
 * The extern decls live inside the macro so they are only seen where sntp.c
 * expands it. */
#define SNTP_SERVER_DNS             1
#define SNTP_SET_SYSTEM_TIME(sec) \
    do { \
        extern volatile unsigned long g_sntp_epoch; \
        extern volatile int           g_sntp_got; \
        g_sntp_epoch = (unsigned long)(sec); \
        g_sntp_got   = 1; \
    } while (0)

/* TESSERAE_TLS_DEBUG turns on just the altcp_tls channel, which is where the
 * SDK's glue logs "mbedtls_ssl_handshake failed: %d". Without it a failed
 * handshake surfaces only as a dead pcb, i.e. "transport error" with no cause.
 * Scoped to one channel on purpose: LWIP_DEBUG with the default channel mask
 * floods the CDC port badly enough to change the timing being debugged. */
#ifdef TESSERAE_TLS_DEBUG
#define LWIP_DEBUG                  1
#define ALTCP_MBEDTLS_DEBUG         LWIP_DBG_ON
#define ALTCP_MBEDTLS_LIB_DEBUG     LWIP_DBG_ON
/* _LEVEL_MIN, not _LEVEL: this is the floor the glue applies to each message,
 * so 0 lets everything mbedTLS emits through. The CEILING is mbedTLS's own
 * global threshold, which defaults to 0 (= silent) and which the SDK's glue
 * never sets -- net_tls.c calls mbedtls_debug_set_threshold() for that. Both
 * are required; setting only one produces no output at all. */
#define ALTCP_MBEDTLS_LIB_DEBUG_LEVEL_MIN 0
/* The SDK's arch/cc.h defines LWIP_PLATFORM_ASSERT but not LWIP_PLATFORM_DIAG,
 * so route debug output at printf (which is stdio-USB here) explicitly rather
 * than relying on lwIP's fallback. */
/* stdio.h rather than a bare printf: lwipopts.h is included by translation
 * units that have not included it, and an implicit declaration of a variadic
 * function is undefined behaviour, not just a warning. */
#include <stdio.h>
#define LWIP_PLATFORM_DIAG(x)       do { printf x; } while (0)
#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_ON
#else
#define LWIP_DEBUG                  0
#endif

#endif /* _LWIPOPTS_H */
