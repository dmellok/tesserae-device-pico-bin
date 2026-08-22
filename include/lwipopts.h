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
#define TCP_WND                     (8 * TCP_MSS)
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

#define LWIP_DEBUG                  0

#endif /* _LWIPOPTS_H */
